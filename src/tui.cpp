#include "nitro/tui.hpp"
#include "nitro/metrics.hpp"
#include "nitro/sampler.hpp"
#include "nitro/trace.hpp"

#include <algorithm>
#include <array>
#include <clocale>
#include <cmath>
#include <curses.h>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace nitro {
namespace {
enum Color : std::uint8_t {
    text = 1,
    muted,
    border,
    cyan,
    violet,
    green,
    amber,
    red,
    selected,
    accent
};
struct Rect {
    int x{};
    int y{};
    int w{};
    int h{};
};
class Terminal {
  public:
    explicit Terminal(int theme) {
        if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO))
            throw std::runtime_error("Interactive mode needs a terminal. Use --json or --headless "
                                     "for redirected output.");
        std::setlocale(LC_ALL, "");
        if (!initscr())
            throw std::runtime_error("Cannot initialize terminal");
        cbreak();
        noecho();
        keypad(stdscr, true);
        curs_set(0);
        set_escdelay(25);
        timeout(50);
        mouseinterval(0);
        mousemask(BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_DOUBLE_CLICKED | BUTTON4_PRESSED |
                      BUTTON5_PRESSED,
                  nullptr);
        colors(theme);
    }
    ~Terminal() {
        mousemask(0, nullptr);
        curs_set(1);
        endwin();
    }
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    static void colors(int theme) {
        if (!has_colors())
            return;
        start_color();
        use_default_colors();
        const short bg = COLORS >= 256 ? 234 : -1;
        const std::array<short, 10> midnight{252, 245, 60, 80, 141, 114, 221, 203, 231, 153};
        const std::array<short, 10> forest{253, 245, 65, 120, 180, 150, 222, 210, 231, 151};
        const std::array<short, 10> basic{COLOR_WHITE,   COLOR_WHITE, COLOR_BLUE,   COLOR_CYAN,
                                          COLOR_MAGENTA, COLOR_GREEN, COLOR_YELLOW, COLOR_RED,
                                          COLOR_WHITE,   COLOR_CYAN};
        for (short i = 1; i <= 10; ++i) {
            short fg = COLORS >= 256
                           ? (theme == 1 ? forest : midnight)[static_cast<std::size_t>(i - 1)]
                           : basic[static_cast<std::size_t>(i - 1)];
            if (theme == 2)
                fg = static_cast<short>(COLORS >= 256 ? (i == muted || i == border ? 245 : 252)
                                                      : COLOR_WHITE);
            init_pair(
                i, fg,
                static_cast<short>(
                    i == selected ? (COLORS >= 256 ? (theme == 2 ? 238 : 24) : COLOR_BLUE) : bg));
        }
        bkgd(COLOR_PAIR(text));
    }
};
void put(int y, int x, std::string_view value, Color color = text, bool bold = false,
         int limit = -1) {
    int height{}, width{};
    getmaxyx(stdscr, height, width);
    if (y < 0 || y >= height || x < 0 || x >= width)
        return;
    const auto clean = safe_text(value);
    const int count = std::min({static_cast<int>(std::min<std::size_t>(clean.size(), 32767)),
                                width - x, limit < 0 ? width : limit});
    attrset(COLOR_PAIR(color) | (bold ? A_BOLD : A_NORMAL));
    mvaddnstr(y, x, clean.c_str(), count);
}
void glyph(int y, int x, wchar_t value, Color color = border) {
    int height{}, width{};
    getmaxyx(stdscr, height, width);
    if (x < 0 || y < 0 || x >= width || y >= height)
        return;
    attrset(COLOR_PAIR(color));
    const wchar_t chars[]{value, 0};
    mvaddnwstr(y, x, chars, 1);
}
void fill(Rect rect, Color color = text) {
    for (int y = rect.y; y < rect.y + rect.h; ++y)
        put(y, rect.x, std::string(static_cast<std::size_t>(std::max(0, rect.w)), ' '), color,
            false, rect.w);
}
void box(Rect r, std::string_view title, bool ascii, bool focus = false) {
    if (r.w < 3 || r.h < 3)
        return;
    const auto color = focus ? cyan : border;
    for (int x = r.x + 1; x < r.x + r.w - 1; ++x) {
        glyph(r.y, x, ascii ? L'-' : L'─', color);
        glyph(r.y + r.h - 1, x, ascii ? L'-' : L'─', color);
    }
    for (int y = r.y + 1; y < r.y + r.h - 1; ++y) {
        glyph(y, r.x, ascii ? L'|' : L'│', color);
        glyph(y, r.x + r.w - 1, ascii ? L'|' : L'│', color);
    }
    glyph(r.y, r.x, ascii ? L'+' : L'╭', color);
    glyph(r.y, r.x + r.w - 1, ascii ? L'+' : L'╮', color);
    glyph(r.y + r.h - 1, r.x, ascii ? L'+' : L'╰', color);
    glyph(r.y + r.h - 1, r.x + r.w - 1, ascii ? L'+' : L'╯', color);
    put(r.y, r.x + 2, " " + std::string{title} + " ", focus ? cyan : accent, true, r.w - 5);
}
std::string pad(const std::string& value, int width, bool right = false) {
    const auto length = static_cast<int>(value.size());
    if (length > width)
        return value.substr(0, static_cast<std::size_t>(std::max(0, width - 1))) + "~";
    const auto spaces = std::string(static_cast<std::size_t>(std::max(0, width - length)), ' ');
    return right ? spaces + value : value + spaces;
}
std::string gbps(std::optional<double> rate) {
    return rate ? format_number(*rate, *rate < 10 ? 2 : 1) : "--";
}
std::string memory(std::uint64_t bytes) {
    constexpr std::array units{"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024 && unit + 1 < units.size()) {
        value /= 1024;
        ++unit;
    }
    return format_number(value, unit > 0 ? 1 : 0) + " " + units[unit];
}
std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
std::optional<double> sum(std::optional<double> a, std::optional<double> b) {
    if (!a && !b)
        return std::nullopt;
    return a.value_or(0) + b.value_or(0);
}
void graph(Rect r, const std::vector<std::optional<double>>& values, double scale, Color color,
           bool ascii) {
    if (r.w <= 0 || r.h <= 0)
        return;
    constexpr std::array<wchar_t, 9> blocks{L' ', L'▁', L'▂', L'▃', L'▄', L'▅', L'▆', L'▇', L'█'};
    const int size = static_cast<int>(values.size());
    const int start = std::max(0, size - r.w);
    const int offset = std::max(0, r.w - size);
    for (int x = 0; x < r.w; ++x) {
        if (x < offset)
            continue;
        const auto& value = values[static_cast<std::size_t>(start + x - offset)];
        if (!value) {
            glyph(r.y + r.h - 1, r.x + x, ascii ? L'.' : L'·', muted);
            continue;
        }
        const double level = std::clamp(*value / std::max(0.001, scale), 0.0, 1.0) * r.h * 8;
        for (int row = 0; row < r.h; ++row) {
            const int amount =
                std::clamp(static_cast<int>(std::round(level - (r.h - row - 1) * 8)), 0, 8);
            if (amount > 0)
                glyph(r.y + row, r.x + x,
                      ascii ? (amount < 4 ? L'.' : L'#') : blocks[static_cast<std::size_t>(amount)],
                      color);
            else if (row == r.h - 1)
                glyph(r.y + row, r.x + x, ascii ? L'_' : L'⎯', border);
        }
    }
}
void meter(int y, int x, int width, double percent, bool ascii, Color color = green) {
    const auto count = static_cast<int>(std::round(std::clamp(percent, 0.0, 100.0) / 100 * width));
    for (int i = 0; i < width; ++i)
        glyph(y, x + i, ascii ? (i < count ? L'|' : L'.') : (i < count ? L'▰' : L'▱'),
              i < count ? color : border);
}
struct Ui {
    Options options;
    PortKind kind{PortKind::rdma};
    int focus{};
    int sort{};
    bool reverse{};
    bool paused{};
    bool average{};
    bool fixed_scale{};
    bool help{};
    bool diagnostics{};
    bool details{};
    bool searching{};
    bool quit{};
    bool record_pending{};
    std::string query;
    std::string previous_query;
    std::string selected_id;
    std::size_t selected_index{};
    std::size_t process_index{};
    std::size_t detail_offset{};
    std::string status;
    std::shared_ptr<const Snapshot> snapshot;
    std::shared_ptr<const Snapshot> live;
    Rect table;
    Rect processes;
    std::size_t table_offset{};
    std::size_t process_offset{};
    std::vector<const PortView*> visible;
    std::vector<const Process*> process_rows;
    [[nodiscard]] std::optional<double> tx(const PortView& port) const {
        return average ? average_rate(port.history, true, std::chrono::seconds{10})
                       : port.tx_gbps();
    }
    [[nodiscard]] std::optional<double> rx(const PortView& port) const {
        return average ? average_rate(port.history, false, std::chrono::seconds{10})
                       : port.rx_gbps();
    }
    [[nodiscard]] const PortView* selected_port() const {
        return visible.empty() ? nullptr : visible[std::min(selected_index, visible.size() - 1)];
    }
    void rows() {
        visible.clear();
        process_rows.clear();
        if (!snapshot)
            return;
        const auto filter = lowercase(query);
        for (const auto& port : snapshot->ports) {
            if (port.port.kind != kind)
                continue;
            if (!filter.empty() &&
                lowercase(port.port.device + " " + port.port.state + " " + port.port.link_layer)
                        .find(filter) == std::string::npos)
                continue;
            visible.push_back(&port);
        }
        std::stable_sort(visible.begin(), visible.end(), [&](const auto* a, const auto* b) {
            int comparison{};
            if (sort == 0)
                comparison = a->port.id().compare(b->port.id());
            else {
                const auto av = sort == 1   ? tx(*a).value_or(-1)
                                : sort == 2 ? rx(*a).value_or(-1)
                                            : a->rate("rx_drops").value_or(-1);
                const auto bv = sort == 1   ? tx(*b).value_or(-1)
                                : sort == 2 ? rx(*b).value_or(-1)
                                            : b->rate("rx_drops").value_or(-1);
                comparison = av > bv ? -1 : av < bv ? 1 : a->port.id().compare(b->port.id());
            }
            return reverse ? comparison > 0 : comparison < 0;
        });
        const auto selected_it = std::find_if(visible.begin(), visible.end(), [&](const auto* p) {
            return p->port.id() == selected_id;
        });
        if (selected_it != visible.end())
            selected_index = static_cast<std::size_t>(std::distance(visible.begin(), selected_it));
        else
            selected_index = std::min(selected_index, visible.empty() ? 0 : visible.size() - 1);
        if (const auto* port = selected_port())
            selected_id = port->port.id();
        for (const auto& process : snapshot->processes) {
            if (filter.empty() || lowercase(process.command + " " + process.device + " " +
                                            process.user + " " + std::to_string(process.pid))
                                          .find(filter) != std::string::npos)
                process_rows.push_back(&process);
        }
        std::stable_sort(process_rows.begin(), process_rows.end(),
                         [](const auto* a, const auto* b) { return a->qp_count > b->qp_count; });
        process_index = std::min(process_index, process_rows.empty() ? 0 : process_rows.size() - 1);
    }
    void move(int delta) {
        auto& index = details ? detail_offset : focus == 1 ? process_index : selected_index;
        const auto size = details      ? (selected_port() ? selected_port()->rates.size() : 0)
                          : focus == 1 ? process_rows.size()
                                       : visible.size();
        if (delta < 0)
            index -= std::min(index, static_cast<std::size_t>(-delta));
        else
            index = std::min(index + static_cast<std::size_t>(delta), size == 0 ? 0 : size - 1);
        if (!details && focus == 0 && selected_port())
            selected_id = selected_port()->port.id();
    }
};
void header(const Ui& ui, int width) {
    put(0, 2, "nitro", cyan, true);
    put(0, 9, "RDMA OBSERVATORY", muted);
    put(0, 29, " 1 RDMA ", ui.kind == PortKind::rdma ? selected : muted, true);
    put(0, 39, " 2 NETWORK ", ui.kind == PortKind::network ? selected : muted, true);
    const std::string mode = ui.options.demo ? "DEMO" : ui.paused ? "PAUSED" : "LIVE";
    put(0, width - 26, mode, ui.options.demo ? amber : ui.paused ? amber : green, true);
    put(0, width - 17, format_number(static_cast<double>(ui.options.interval_ms) / 1000, 1) + "s",
        muted);
    put(0, width - 10, "h help", muted);
}
void draw_traffic(const Ui& ui, Rect r) {
    box(r, ui.kind == PortKind::rdma ? "fabric throughput" : "network throughput",
        ui.options.ascii);
    put(r.y, r.x + r.w - 22, ui.average ? " 10s average " : " instantaneous ", muted, false, 19);
    std::vector<std::optional<double>> tx_values(history_limit), rx_values(history_limit);
    std::optional<double> total_tx, total_rx;
    double link_sum = 0;
    double scale = 1;
    double span_seconds = 0;
    for (const auto* port : ui.visible) {
        total_tx = sum(total_tx, ui.tx(*port));
        total_rx = sum(total_rx, ui.rx(*port));
        link_sum += port->port.link_gbps.value_or(0);
        const auto& hist = port->history;
        if (hist.size() > 1) {
            const auto count =
                std::min<std::size_t>(hist.size(), static_cast<std::size_t>(std::max(1, r.w - 4)));
            span_seconds =
                std::max(span_seconds, std::chrono::duration<double>(hist.back().time -
                                                                     hist[hist.size() - count].time)
                                           .count());
        }
        for (std::size_t i = 0; i < hist.size(); ++i) {
            const auto target = history_limit - hist.size() + i;
            tx_values[target] = sum(tx_values[target], hist[i].tx);
            rx_values[target] = sum(rx_values[target], hist[i].rx);
        }
    }
    for (const auto& value : tx_values)
        scale = std::max(scale, value.value_or(0));
    for (const auto& value : rx_values)
        scale = std::max(scale, value.value_or(0));
    scale = ui.fixed_scale && link_sum > 0 ? link_sum : scale * 1.1;
    const int gh = std::max(1, (r.h - 6) / 2);
    const int rx_y = r.y + 3 + gh;
    put(r.y + 1, r.x + 2, "TX", cyan, true);
    put(r.y + 1, r.x + 6, gbps(total_tx) + " Gbps", cyan, true);
    put(r.y + 1, r.x + r.w - 22, "max " + gbps(scale) + " Gbps", muted, false, 19);
    graph({r.x + 2, r.y + 2, r.w - 4, gh}, tx_values, scale, cyan, ui.options.ascii);
    put(rx_y, r.x + 2, "RX", violet, true);
    put(rx_y, r.x + 6, gbps(total_rx) + " Gbps", violet, true);
    graph({r.x + 2, rx_y + 1, r.w - 4, gh}, rx_values, scale, violet, ui.options.ascii);
    put(r.y + r.h - 2, r.x + 2, "< " + format_number(span_seconds, 0) + "s history", muted);
    put(r.y + r.h - 2, r.x + r.w - 17, ui.fixed_scale ? "line-rate scale" : "auto scale   >", muted,
        false, 15);
}
void draw_host(const Ui& ui, Rect r) {
    box(r, "system", ui.options.ascii);
    if (!ui.snapshot)
        return;
    const auto& snap = *ui.snapshot;
    put(r.y + 1, r.x + 2, snap.host.name, text, true, r.w - 4);
    std::size_t active = 0;
    for (const auto* port : ui.visible)
        if (port->port.state == "ACTIVE" || port->port.state == "up")
            ++active;
    put(r.y + 3, r.x + 2, "LINKS", muted);
    put(r.y + 3, r.x + 10,
        std::to_string(active) + " / " + std::to_string(ui.visible.size()) + " active", green, true,
        r.w - 12);
    std::optional<double> drops;
    for (const auto* port : ui.visible)
        drops = sum(drops, port->rate("rx_drops"));
    put(r.y + 4, r.x + 2, "DROPS", muted);
    put(r.y + 4, r.x + 10, format_rate(drops), drops.value_or(0) > 0 ? red : green);
    put(r.y + 5, r.x + 2, "LOAD", muted);
    put(r.y + 5, r.x + 10, format_number(snap.host.load, 2), text);
    const double used = snap.host.memory_total > 0
                            ? 100 * (1.0 - static_cast<double>(snap.host.memory_available) /
                                               static_cast<double>(snap.host.memory_total))
                            : 0;
    put(r.y + 7, r.x + 2, "RAM " + format_number(used, 0) + "%", muted);
    meter(r.y + 7, r.x + 11, r.w - 14, used, ui.options.ascii, violet);
    if (r.h > 10) {
        put(r.y + 9, r.x + 2, "UP " + format_number(snap.host.uptime_seconds / 86400, 1) + "d",
            muted);
        put(r.y + 10, r.x + 2, "poll " + format_number(snap.collection_ms, 1) + " ms", muted);
    }
}
void draw_ports(Ui& ui, Rect r) {
    ui.table = r;
    constexpr std::array sorts{"name", "TX", "RX", "drops"};
    box(r, ui.kind == PortKind::rdma ? "1 interfaces" : "2 interfaces", ui.options.ascii,
        ui.focus == 0);
    put(r.y, r.x + r.w - 26,
        " sort: " + std::string{sorts[static_cast<std::size_t>(ui.sort)]} +
            ((ui.sort == 0) != ui.reverse ? " asc " : " desc "),
        muted, false, 23);
    const int name_x = r.x + 3, state_x = r.x + 22, tx_x = r.x + 33, rx_x = r.x + 45,
              util_x = r.x + 58;
    put(r.y + 1, name_x, "INTERFACE / PORT", muted);
    put(r.y + 1, state_x, "STATE", muted);
    put(r.y + 1, tx_x, "TX Gbps", cyan);
    put(r.y + 1, rx_x, "RX Gbps", violet);
    put(r.y + 1, util_x, "UTILIZATION", muted);
    if (r.w >= 98) {
        put(r.y + 1, r.x + 75, "TX pkt/s", muted);
        put(r.y + 1, r.x + 87, "RX pkt/s", muted);
    }
    if (r.w >= 112)
        put(r.y + 1, r.x + 101, "DROP/s", muted);
    const auto capacity = static_cast<std::size_t>(std::max(0, r.h - 4));
    if (ui.selected_index < ui.table_offset)
        ui.table_offset = ui.selected_index;
    if (capacity > 0 && ui.selected_index >= ui.table_offset + capacity)
        ui.table_offset = ui.selected_index - capacity + 1;
    if (ui.visible.empty()) {
        put(r.y + 3, r.x + 3,
            ui.query.empty() ? (ui.kind == PortKind::rdma ? "No RDMA interfaces detected."
                                                          : "No network interfaces detected.")
                             : "No interfaces match this filter.",
            text, false, r.w - 6);
        if (r.h >= 7)
            put(r.y + 4, r.x + 3,
                ui.query.empty() ? "Press 1/2 to switch views, or launch nitro --demo to explore."
                                 : "Press / to edit the filter, or Esc to clear it.",
                muted, false, r.w - 6);
    }
    for (std::size_t i = ui.table_offset; i < ui.visible.size() && i - ui.table_offset < capacity;
         ++i) {
        const auto& port = *ui.visible[i];
        const bool current = i == ui.selected_index;
        const auto color = current && ui.focus == 0 ? selected : text;
        const int y = r.y + 2 + static_cast<int>(i - ui.table_offset);
        if (current && ui.focus == 0)
            fill({r.x + 1, y, r.w - 2, 1}, selected);
        put(y, r.x + 1, current ? ">" : " ", current ? cyan : text, true);
        put(y, name_x,
            pad(port.port.device +
                    (port.port.number > 0 ? " / " + std::to_string(port.port.number) : ""),
                18),
            color, current);
        put(y, state_x, pad(port.port.state, 9),
            current && ui.focus == 0    ? selected
            : port.port.state == "DOWN" ? red
                                        : green);
        put(y, tx_x, pad(gbps(ui.tx(port)), 9, true), current && ui.focus == 0 ? selected : cyan,
            current);
        put(y, rx_x, pad(gbps(ui.rx(port)), 9, true), current && ui.focus == 0 ? selected : violet,
            current);
        const auto maximum = std::max(ui.tx(port).value_or(0), ui.rx(port).value_or(0));
        const double utilization = port.port.link_gbps && *port.port.link_gbps > 0
                                       ? maximum * 100 / *port.port.link_gbps
                                       : 0;
        if (port.port.link_gbps && (ui.tx(port) || ui.rx(port))) {
            meter(y, util_x, 7, utilization, ui.options.ascii, utilization > 90 ? amber : green);
            put(y, util_x + 8, pad(format_number(utilization, 0) + "%", 5, true), color);
        } else
            put(y, util_x, "--", muted);
        if (r.w >= 98) {
            put(y, r.x + 75, pad(format_rate(port.rate("tx_pkts")), 10, true), color);
            put(y, r.x + 87, pad(format_rate(port.rate("rx_pkts")), 10, true), color);
        }
        if (r.w >= 112)
            put(y, r.x + 100, pad(format_rate(port.rate("rx_drops")), 10, true),
                port.rate("rx_drops").value_or(0) > 0 ? red : color);
    }
    put(r.y + r.h - 2, r.x + 3,
        std::to_string(ui.visible.size()) + " ports  |  Enter inspect  / filter", muted, false,
        r.w - 6);
}
void draw_processes(Ui& ui, Rect r) {
    ui.processes = r;
    box(r, "processes / queue pairs", ui.options.ascii, ui.focus == 1);
    put(r.y + 1, r.x + 2, "PID     DEVICE       QPs   RSS        COMMAND", muted, false, r.w - 4);
    const auto capacity = static_cast<std::size_t>(std::max(0, r.h - 4));
    if (ui.process_index < ui.process_offset)
        ui.process_offset = ui.process_index;
    if (capacity && ui.process_index >= ui.process_offset + capacity)
        ui.process_offset = ui.process_index - capacity + 1;
    for (std::size_t i = ui.process_offset;
         i < ui.process_rows.size() && i - ui.process_offset < capacity; ++i) {
        const auto& p = *ui.process_rows[i];
        const int y = r.y + 2 + static_cast<int>(i - ui.process_offset);
        const auto color = ui.focus == 1 && i == ui.process_index ? selected : text;
        if (color == selected)
            fill({r.x + 1, y, r.w - 2, 1}, selected);
        put(y, r.x + 2,
            pad(p.pid ? std::to_string(p.pid) : "kernel", 7) + " " + pad(p.device, 12) + " " +
                pad(std::to_string(p.qp_count), 4, true) + "  " + pad(memory(p.rss_bytes), 10) +
                " " + safe_text(p.command),
            color, false, r.w - 4);
    }
    if (ui.process_rows.empty())
        put(r.y + 3, r.x + 2, "No visible RDMA process owners.", muted, false, r.w - 4);
    put(r.y + r.h - 2, r.x + 2,
        std::to_string(ui.process_rows.size()) + " process/device pairs  |  refreshed every 3s",
        muted, false, r.w - 4);
}
void draw_inspector(const Ui& ui, Rect r) {
    const auto* port = ui.selected_port();
    box(r, "selected port", ui.options.ascii);
    if (!port) {
        put(r.y + 2, r.x + 2, "Select an interface to inspect", muted, false, r.w - 4);
        return;
    }
    put(r.y + 1, r.x + 2, port->port.device + " / " + std::to_string(port->port.number), text, true,
        r.w - 4);
    put(r.y + 2, r.x + 2, port->port.link_layer + "  " + gbps(port->port.link_gbps) + " Gbps",
        muted, false, r.w - 4);
    put(r.y + 4, r.x + 2, "RDMA read", muted);
    put(r.y + 4, r.x + 15, format_rate(port->rate("rdma_read_bytes"), true), cyan, false, r.w - 17);
    put(r.y + 5, r.x + 2, "RDMA write", muted);
    put(r.y + 5, r.x + 15, format_rate(port->rate("rdma_write_bytes"), true), violet, false,
        r.w - 17);
    if (r.h > 8) {
        put(r.y + 6, r.x + 2, "Retransmits", muted);
        put(r.y + 6, r.x + 15, format_rate(port->rate("retrans_pkts")), amber);
    }
    put(r.y + r.h - 2, r.x + 2,
        "Enter: " + std::to_string(port->rates.size()) + " hardware counters", muted, false,
        r.w - 4);
}
void help_overlay(const Ui& ui, int width, int height) {
    const Rect r{std::max(0, (width - 74) / 2), std::max(1, (height - 25) / 2), std::min(74, width),
                 std::min(25, height - 2)};
    fill(r);
    box(r, "nitro / keyboard & mouse", ui.options.ascii, true);
    const std::array<std::pair<std::string_view, std::string_view>, 19> lines{
        {{"1 / 2", "RDMA fabric / network interfaces"},
         {"Up Down / j k", "Select row; scroll counter details"},
         {"PageUp PageDown", "Move by eight rows"},
         {"Tab", "Focus interfaces or processes"},
         {"Enter", "Inspect all counters / process details"},
         {"/", "Filter by device, state, PID or command"},
         {"s / S", "Cycle interface sort / reverse order"},
         {"Space", "Pause display (sampling and recording continue)"},
         {"a", "Toggle time-weighted 10-second average"},
         {"g", "Automatic / combined line-rate graph scale"},
         {"+ / -", "Faster / slower sample interval"},
         {"t", "Cycle midnight, forest, monochrome themes"},
         {"w", "Show collector diagnostics"},
         {"r", "Start / stop a Perfetto recording"},
         {"Mouse", "Click rows and tabs; wheel to scroll"},
         {"Esc", "Close dialog / clear filter"},
         {"q / Ctrl-C", "Quit and finalize recording"},
         {"--", "Metric unavailable, warming up, or counter reset"},
         {"DEMO", "Simulated data; enabled only with --demo"}}};
    for (std::size_t i = 0; i < lines.size() && static_cast<int>(i) + 3 < r.h - 1; ++i) {
        put(r.y + 2 + static_cast<int>(i), r.x + 3, lines[i].first, cyan, true, 19);
        put(r.y + 2 + static_cast<int>(i), r.x + 23, lines[i].second, text, false, r.w - 26);
    }
    put(r.y + r.h - 2, r.x + 3, "h / Esc close    nitro " NITRO_VERSION, muted);
}
void detail_overlay(Ui& ui, int width, int height) {
    const Rect r{2, 2, width - 4, height - 4};
    fill(r);
    box(r, "inspector / Esc to close", ui.options.ascii, true);
    if (ui.focus == 1) {
        if (ui.process_rows.empty())
            return;
        const auto& p = *ui.process_rows[ui.process_index];
        const std::array<std::string, 8> lines{
            "PID        " + std::to_string(p.pid) + "    " + p.user,
            "Device     " + p.device,
            "Queue pairs " + std::to_string(p.qp_count),
            "State      " + p.state + "    nice " + std::to_string(p.nice),
            "Memory     " + memory(p.rss_bytes) + " resident / " + memory(p.virtual_bytes) +
                " virtual",
            "Threads    " + std::to_string(p.threads),
            "Command",
            safe_text(p.command)};
        for (std::size_t i = 0; i < lines.size(); ++i)
            put(r.y + 2 + static_cast<int>(i), r.x + 3, lines[i], i == 7 ? cyan : text, false,
                r.w - 6);
        put(r.y + r.h - 2, r.x + 3,
            "QPs identify ownership. Per-process bandwidth is not inferred.", muted, false,
            r.w - 6);
        return;
    }
    const auto* port = ui.selected_port();
    if (!port)
        return;
    put(r.y + 2, r.x + 3,
        port->port.device + " / " + std::to_string(port->port.number) + "   " + port->port.state +
            "   " + port->port.link_layer + "   " + gbps(port->port.link_gbps) + " Gbps",
        cyan, true, r.w - 6);
    put(r.y + 3, r.x + 3,
        "Source: " + port->port.source + "   Raw totals retain driver units; *_bytes are bytes.",
        muted, false, r.w - 6);
    put(r.y + 5, r.x + 3, "COUNTER", muted);
    put(r.y + 5, r.x + r.w - 36, "TOTAL", muted);
    put(r.y + 5, r.x + r.w - 15, "RATE / SEC", muted);
    const auto capacity = static_cast<std::size_t>(std::max(1, r.h - 9));
    ui.detail_offset = std::min(ui.detail_offset,
                                port->rates.size() > capacity ? port->rates.size() - capacity : 0);
    std::size_t index = 0;
    for (const auto& [name, value] : port->rates) {
        if (index++ < ui.detail_offset)
            continue;
        const auto row = index - ui.detail_offset - 1;
        if (row >= capacity)
            break;
        const int y = r.y + 6 + static_cast<int>(row);
        put(y, r.x + 3, name, text, false, r.w - 41);
        put(y, r.x + r.w - 39, pad(std::to_string(value.total), 22, true), muted);
        put(y, r.x + r.w - 15,
            pad(value.reset ? "reset" : format_rate(value.per_second, name.ends_with("bytes")), 12,
                true),
            value.reset ? amber : cyan);
    }
    put(r.y + r.h - 2, r.x + 3,
        std::to_string(port->rates.size()) +
            " counters  |  Up/Down scroll  |  unavailable rates shown as --",
        muted, false, r.w - 6);
}
void draw(Ui& ui) {
    int height{}, width{};
    getmaxyx(stdscr, height, width);
    erase();
    if (width < 80 || height < 24) {
        put(1, 2, "nitro", cyan, true);
        put(3, 2, "Resize terminal to at least 80 x 24", text, false, width - 3);
        put(5, 2, "Current: " + std::to_string(width) + " x " + std::to_string(height), muted,
            false, width - 3);
        put(7, 2, "q quit | sampling continues", muted, false, width - 3);
        wnoutrefresh(stdscr);
        doupdate();
        return;
    }
    header(ui, width);
    const int top_height = height >= 36 ? 13 : 10;
    const int host_width = std::max(28, width / 4);
    draw_traffic(ui, {0, 2, width - host_width, top_height});
    draw_host(ui, {width - host_width, 2, host_width, top_height});
    const int middle_y = 2 + top_height;
    const bool lower = height >= 32;
    const int table_height =
        lower ? std::max(8, (height - middle_y - 2) / 2) : height - middle_y - 2;
    draw_ports(ui, {0, middle_y, width, table_height});
    if (lower) {
        const int y = middle_y + table_height;
        const int inspector_width = std::max(34, width / 3);
        draw_processes(ui, {0, y, width - inspector_width, height - y - 2});
        draw_inspector(ui, {width - inspector_width, y, inspector_width, height - y - 2});
    } else
        ui.processes = {};
    if (ui.searching) {
        put(height - 2, 2, "Filter: " + ui.query + "_   Enter apply / Esc cancel", cyan, false,
            width - 4);
    } else if (!ui.status.empty())
        put(height - 2, 2, ui.status, amber, false, width - 4);
    else if (ui.live && !ui.live->recording_error.empty())
        put(height - 2, 2, "Recording: " + ui.live->recording_error, red, false, width - 4);
    else if (ui.live && ui.live->recording)
        put(height - 2, 2,
            "REC  " + ui.live->recording_path + (ui.paused ? "  [display paused]" : "  [r stop]"),
            red, true, width - 4);
    else if (!ui.paused && ui.snapshot &&
             Clock::now() - ui.snapshot->taken_at >
                 std::chrono::milliseconds{std::max(3000, ui.options.interval_ms * 3)})
        put(height - 2, 2, "STALE: waiting for the collector", amber);
    else if (ui.snapshot && !ui.snapshot->warnings.empty())
        put(height - 2, 2, ui.snapshot->warnings.front() + "  [w diagnostics]", amber, false,
            width - 4);
    else if (ui.paused)
        put(height - 2, 2, "PAUSED  Sampling and recording continue. Space to resume.", amber);
    else if (!ui.query.empty())
        put(height - 2, 2, "Filter: " + ui.query + "  [Esc clear]", cyan);
    else
        put(height - 2, 2,
            ui.options.demo ? "DEMO FABRIC  /  simulated traffic, devices and processes"
                            : "RDMA counters / network traffic / process ownership",
            muted, false, width - 4);
    put(height - 1, 2,
        "1/2 view  Tab focus  / filter  s sort  Enter inspect  r record  +/- rate  q quit", muted,
        false, width - 3);
    if (ui.details)
        detail_overlay(ui, width, height);
    if (ui.diagnostics) {
        const Rect r{2, 2, width - 4, height - 4};
        fill(r);
        box(r, "diagnostics / w or Esc to close", ui.options.ascii, true);
        put(r.y + 2, r.x + 3,
            "Collector: " +
                std::string{ui.options.demo ? "demo" : "Linux RDMA netlink + sysfs + procfs"},
            cyan, true);
        if (ui.live) {
            put(r.y + 4, r.x + 3,
                "Last poll: " + format_number(ui.live->collection_ms, 2) + " ms / sample " +
                    std::to_string(ui.live->sequence),
                muted);
            put(r.y + 5, r.x + 3, "QPs are refreshed every 3s. Process rows are device-scoped.",
                muted);
            int y = r.y + 7;
            for (const auto& warning : ui.live->warnings) {
                if (y >= r.y + r.h - 2)
                    break;
                put(y++, r.x + 3, warning, amber, false, r.w - 6);
            }
            if (ui.live->warnings.empty())
                put(y, r.x + 3, "No collector warnings.", green);
        }
    }
    if (ui.help)
        help_overlay(ui, width, height);
    wnoutrefresh(stdscr);
    doupdate();
}
void input(Ui& ui, Sampler& sampler, int key) {
    if (key == ERR || key == KEY_RESIZE)
        return;
    ui.status.clear();
    if (ui.searching) {
        if (key == 27) {
            ui.query = ui.previous_query;
            ui.searching = false;
        } else if (key == '\n' || key == KEY_ENTER)
            ui.searching = false;
        else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
            if (!ui.query.empty())
                ui.query.pop_back();
        } else if (key >= 32 && key < 127 && ui.query.size() < 128)
            ui.query += static_cast<char>(key);
        ui.rows();
        return;
    }
    if (key == 'q' || key == 3) {
        ui.quit = true;
        return;
    }
    if (key == 'h' || key == '?') {
        ui.help = !ui.help;
        return;
    }
    if (key == 'w') {
        ui.diagnostics = !ui.diagnostics;
        return;
    }
    if (ui.diagnostics) {
        if (key == 27)
            ui.diagnostics = false;
        return;
    }
    if (ui.help) {
        if (key == 27)
            ui.help = false;
        return;
    }
    if (key == 27) {
        if (ui.details)
            ui.details = false;
        else {
            ui.query.clear();
            ui.rows();
        }
        return;
    }
    if (key == KEY_MOUSE) {
        MEVENT mouse{};
        if (getmouse(&mouse) != OK)
            return;
        if ((mouse.bstate & BUTTON4_PRESSED) != 0) {
            ui.move(-1);
            return;
        }
        if ((mouse.bstate & BUTTON5_PRESSED) != 0) {
            ui.move(1);
            return;
        }
        if ((mouse.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_DOUBLE_CLICKED)) != 0) {
            if (mouse.y == 0 && mouse.x >= 29 && mouse.x <= 49) {
                ui.kind = mouse.x < 39 ? PortKind::rdma : PortKind::network;
                ui.selected_id.clear();
                ui.selected_index = 0;
                ui.rows();
            } else if (!ui.details && mouse.y >= ui.table.y + 2 &&
                       mouse.y < ui.table.y + ui.table.h - 2) {
                const auto index =
                    ui.table_offset + static_cast<std::size_t>(mouse.y - ui.table.y - 2);
                if (index < ui.visible.size()) {
                    ui.focus = 0;
                    ui.selected_index = index;
                    ui.selected_id = ui.visible[index]->port.id();
                }
            } else if (!ui.details && mouse.y >= ui.processes.y + 2 &&
                       mouse.y < ui.processes.y + ui.processes.h - 2 && mouse.x < ui.processes.w) {
                const auto index =
                    ui.process_offset + static_cast<std::size_t>(mouse.y - ui.processes.y - 2);
                if (index < ui.process_rows.size()) {
                    ui.focus = 1;
                    ui.process_index = index;
                }
            }
            if ((mouse.bstate & BUTTON1_DOUBLE_CLICKED) != 0) {
                ui.details = true;
                ui.detail_offset = 0;
            }
        }
        return;
    }
    switch (key) {
    case KEY_UP:
    case 'k':
        ui.move(-1);
        break;
    case KEY_DOWN:
    case 'j':
        ui.move(1);
        break;
    case KEY_PPAGE:
        ui.move(-8);
        break;
    case KEY_NPAGE:
        ui.move(8);
        break;
    case KEY_HOME:
        ui.move(-100000);
        break;
    case KEY_END:
        ui.move(100000);
        break;
    case '\t':
        ui.focus = 1 - ui.focus;
        ui.detail_offset = 0;
        break;
    case '\n':
    case KEY_ENTER:
        ui.details = !ui.details;
        ui.detail_offset = 0;
        break;
    case '1':
    case '2':
        ui.kind = key == '1' ? PortKind::rdma : PortKind::network;
        ui.selected_id.clear();
        ui.selected_index = 0;
        ui.details = false;
        ui.focus = 0;
        ui.rows();
        break;
    case '/':
        ui.searching = true;
        ui.previous_query = ui.query;
        break;
    case ' ':
        ui.paused = !ui.paused;
        break;
    case 'a':
        ui.average = !ui.average;
        break;
    case 'g':
        ui.fixed_scale = !ui.fixed_scale;
        break;
    case 's':
        ui.sort = (ui.sort + 1) % 4;
        ui.rows();
        break;
    case 'S':
        ui.reverse = !ui.reverse;
        ui.rows();
        break;
    case 't':
        ui.options.theme = (ui.options.theme + 1) % 3;
        Terminal::colors(ui.options.theme);
        break;
    case '+':
    case '=':
        ui.options.interval_ms = std::max(100, ui.options.interval_ms - 100);
        sampler.interval(ui.options.interval_ms);
        break;
    case '-':
        ui.options.interval_ms = std::min(60000, ui.options.interval_ms + 100);
        sampler.interval(ui.options.interval_ms);
        break;
    case 'r': {
        const auto current = sampler.latest();
        if (!ui.record_pending) {
            const bool recording = current && current->recording;
            sampler.record(recording ? "" : recording_filename());
            ui.status = recording ? "Finalizing recording..." : "Starting recording...";
            ui.record_pending = true;
        }
        break;
    }
    default:
        break;
    }
}
} // namespace
int run_tui(const Options& options) {
    Terminal terminal(options.theme);
    Sampler sampler(options);
    Ui ui;
    ui.options = options;
    if (MB_CUR_MAX == 1)
        ui.options.ascii = true;
    auto last_draw = Clock::time_point{};
    std::shared_ptr<const Snapshot> observed;
    bool dirty = true;
    while (!ui.quit && !interrupted) {
        const auto latest = sampler.latest();
        if (latest && latest != observed) {
            observed = latest;
            ui.live = latest;
            dirty = true;
            ui.record_pending = false;
            if (ui.status == "Finalizing recording..." || ui.status == "Starting recording...")
                ui.status.clear();
            if (!ui.paused || !ui.snapshot) {
                ui.snapshot = latest;
                ui.rows();
                dirty = true;
            }
        }
        if (!ui.paused && latest && ui.snapshot != latest) {
            ui.snapshot = latest;
            ui.rows();
            dirty = true;
        }
        if (dirty || Clock::now() - last_draw >= std::chrono::seconds{1}) {
            draw(ui);
            dirty = false;
            last_draw = Clock::now();
        }
        const auto key = getch();
        if (key != ERR) {
            input(ui, sampler, key);
            dirty = true;
        }
    }
    return 0;
}
} // namespace nitro
