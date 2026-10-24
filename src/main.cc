// Notes.xapp — Quick-note scratchpad for FreeBSD-X
//
// Layout:
//   Left panel (240px):  search bar + note list + "+ New Note" button
//   Right panel (flex):  note editor (title, color, body) or empty state
//
// Features:
//   • Notes sorted: pinned first, then by last-modified descending
//   • Search filters list in real time (server-side substring on title+body)
//   • Editor: title, body (up to 164 chars), color label (5 options), pin toggle
//   • Save button commits via Add or Update opcode
//   • Delete button removes the current note
//   • NoteChanged broadcast keeps list live across app instances

#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"
#include "shared/protocol.h"
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <ctime>
#include <functional>
#include <algorithm>
#include <sstream>

using namespace fbx::xkit;
using namespace fbx::shared;

// ── Local note model ──────────────────────────────────────────────────────────

struct LocalNote {
    uint32_t    id{};
    uint32_t    created{};
    uint32_t    modified{};
    bool        pinned{false};
    uint8_t     color{0};
    std::string title;
    std::string body;
};

static LocalNote fromEntry(const NoteEntry& e) {
    LocalNote n;
    n.id       = e.id_;
    n.created  = e.created_;
    n.modified = e.modified_;
    n.pinned   = e.pinned_ != 0;
    n.color    = e.color_;
    n.title    = e.title_;
    n.body     = e.body_;
    return n;
}

[[maybe_unused]] static void toEntry(NoteEntry& e, const LocalNote& n) {
    std::memset(&e, 0, sizeof(e));
    e.id_       = n.id;
    e.created_  = n.created;
    e.modified_ = n.modified;
    e.pinned_   = n.pinned ? 1 : 0;
    e.color_    = n.color;
    std::strncpy(e.title_, n.title.c_str(), sizeof(e.title_) - 1);
    std::strncpy(e.body_,  n.body.c_str(),  sizeof(e.body_)  - 1);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

// Color accent backgrounds for the 5 label options.
struct ColorDef { float r, g, b; const char* name; };
static constexpr ColorDef kColors[5] = {
    {0.32f, 0.32f, 0.32f, "Default"},
    {0.78f, 0.70f, 0.08f, "Yellow" },
    {0.18f, 0.62f, 0.28f, "Green"  },
    {0.14f, 0.44f, 0.84f, "Blue"   },
    {0.84f, 0.22f, 0.50f, "Pink"   },
};

[[maybe_unused]] static std::string formatTimestamp(uint32_t ts) {
    if (ts == 0) return "";
    std::time_t t = static_cast<std::time_t>(ts);
    char buf[32];
    std::tm* tm = std::localtime(&t);
    if (!tm) return "";
    std::strftime(buf, sizeof(buf), "%b %d, %Y  %H:%M", tm);
    return buf;
}

// First non-empty line of body for list preview (up to 50 chars).
static std::string bodyPreview(const std::string& body) {
    std::string first;
    for (char c : body) {
        if (c == '\n') break;
        first += c;
    }
    if (first.size() > 50) first = first.substr(0, 47) + "…";
    return first;
}

// ── App ───────────────────────────────────────────────────────────────────────

int main(int /*argc*/, char** /*argv*/) {

    // ── Shared state ─────────────────────────────────────────────────────────
    auto notes        = std::make_shared<std::vector<LocalNote>>();
    auto selected_id  = std::make_shared<uint32_t>(0);
    auto search_query = std::make_shared<std::string>("");

    // Editor state (for the currently open note, before Save)
    auto ed_id     = std::make_shared<uint32_t>(0);    // 0 = unsaved new note
    auto ed_title  = std::make_shared<std::string>("");
    auto ed_body   = std::make_shared<std::string>("");
    auto ed_pinned = std::make_shared<bool>(false);
    auto ed_color  = std::make_shared<uint8_t>(0);

    // Widget forward declarations
    auto list_col    = std::make_shared<std::shared_ptr<Widget>>();
    auto right_pane  = std::make_shared<std::shared_ptr<Widget>>();
    auto editor_col  = std::make_shared<std::shared_ptr<Widget>>();
    auto empty_label = std::make_shared<std::shared_ptr<Widget>>();

    // Rebuild lambdas — forward-declared
    auto rebuild_list  = std::make_shared<std::function<void()>>();
    auto load_page     = std::make_shared<std::function<void(uint32_t)>>();
    auto open_note     = std::make_shared<std::function<void(uint32_t)>>();
    auto new_note      = std::make_shared<std::function<void()>>();
    auto rebuild_editor= std::make_shared<std::function<void()>>();

    // ── Root layout: flex row ─────────────────────────────────────────
    auto root_row = std::make_shared<FlexRow>();
    root_row->setFixedSize(780, 560);

    // ══ LEFT PANEL ════════════════════════════════════════════════════
    auto left = std::make_shared<FlexColumn>();
    left->setFixedSize(240, 0.0f);
    left->setPadding(12);
    left->setGap(8);
    left->setBackgroundColor(Color{0.12f, 0.12f, 0.12f, 1.0f});
    root_row->addChild(left);

    // "Notes" header
    auto header_label = std::make_shared<TextWidget>("Notes", 18.0f);
    header_label->setFontWeight(700);
    header_label->setColor(1.0f, 1.0f, 1.0f, 1.0f);
    left->addChild(header_label);

    // Search bar
    auto search_input = std::make_shared<TextInputWidget>("");
    search_input->setPlaceholder("Search…");
    search_input->setFixedSize(0.0f, 30);
    search_input->setOnChanged([search_query, load_page, notes](const std::string& q) {
        *search_query = q;
        notes->clear();
        if (*load_page) (*load_page)(0);
    });
    left->addChild(search_input);

    // Note list scroll
    auto list_column = std::make_shared<FlexColumn>();
    list_column->setGap(3);
    *list_col = list_column;

    auto list_scroll = std::make_shared<ScrollViewWidget>(list_column);
    list_scroll->setFlexGrow(1.0f);
    left->addChild(list_scroll);

    // "+ New Note" button
    auto new_btn = std::make_shared<ButtonWidget>("+ New Note");
    new_btn->setFixedSize(0.0f, 34);
    new_btn->setBackgroundColor(Color{0.18f, 0.48f, 0.88f, 1.0f});
    new_btn->onClick([new_note]() {
        if (*new_note) (*new_note)();
    });
    left->addChild(new_btn);

    // ══ RIGHT PANEL ═══════════════════════════════════════════════════
    auto right = std::make_shared<FlexColumn>();
    right->setFlexGrow(1.0f);
    right->setBackgroundColor(Color{0.10f, 0.10f, 0.10f, 1.0f});
    root_row->addChild(right);
    *right_pane = right;

    // Empty state placeholder
    auto empty_lbl = std::make_shared<TextWidget>("Select a note or create a new one", 14.0f);
    empty_lbl->setColor(0.45f, 0.45f, 0.45f, 1.0f);
    empty_lbl->setPadding(40);
    right->addChild(empty_lbl);
    *empty_label = empty_lbl;

    // ══ REBUILD EDITOR ════════════════════════════════════════════════
    // Rebuilds the right panel for the current ed_* state.
    *rebuild_editor = [=]() {
        auto rp = std::static_pointer_cast<FlexColumn>(*right_pane);
        if (!rp) return;
        rp->clearChildren();

        // ── Editor header bar ─────────────────────────────────────────
        auto hdr = std::make_shared<FlexRow>();
        hdr->setFixedSize(0.0f, 44);
        hdr->setPadding(8);
        hdr->setGap(8);
        hdr->setBackgroundColor(Color{0.14f, 0.14f, 0.14f, 1.0f});
        hdr->setCrossAxisAlignment(CrossAxisAlignment::Center);
        rp->addChild(hdr);

        // Pin toggle
        auto pin_btn = std::make_shared<ButtonWidget>(*ed_pinned ? "📌 Pinned" : "Pin");
        pin_btn->setFixedSize(72, 28);
        if (*ed_pinned)
            pin_btn->setBackgroundColor(Color{0.78f, 0.55f, 0.08f, 1.0f});
        else
            pin_btn->setBackgroundColor(Color{0.25f, 0.25f, 0.25f, 1.0f});
        pin_btn->onClick([ed_pinned, rebuild_editor]() {
            *ed_pinned = !*ed_pinned;
            if (*rebuild_editor) (*rebuild_editor)();
        });
        hdr->addChild(pin_btn);

        // Color picker (5 small buttons)
        for (uint8_t ci = 0; ci < 5; ++ci) {
            auto cb = std::make_shared<ButtonWidget>(kColors[ci].name);
            cb->setFixedSize(58, 28);
            float alpha = (*ed_color == ci) ? 1.0f : 0.45f;
            cb->setBackgroundColor(Color{kColors[ci].r * alpha,
                                         kColors[ci].g * alpha,
                                         kColors[ci].b * alpha,
                                         1.0f});
            uint8_t idx = ci;
            cb->onClick([ed_color, rebuild_editor, idx]() {
                *ed_color = idx;
                if (*rebuild_editor) (*rebuild_editor)();
            });
            hdr->addChild(cb);
        }

        // Spacer
        auto spacer = std::make_shared<FlexRow>();
        spacer->setFlexGrow(1.0f);
        hdr->addChild(spacer);

        // Delete button (hidden for unsaved new notes)
        if (*ed_id != 0) {
            auto del_btn = std::make_shared<ButtonWidget>("Delete");
            del_btn->setFixedSize(64, 28);
            del_btn->setBackgroundColor(Color{0.72f, 0.15f, 0.15f, 1.0f});
            del_btn->onClick([ed_id, selected_id, notes, rebuild_list,
                              right_pane, empty_label, rebuild_editor]() {
                uint32_t del_id = *ed_id;
                NotesIdRequest req{};
                req.id_ = del_id;
                auto* xbus = XApplication::getBus();
                if (xbus) {
                    xbus->Call("com.freebsdx.Notes",
                               static_cast<uint32_t>(NotesOpcode::Delete),
                               {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                               [](const XBusMessage&){});
                }
                *ed_id = 0;
                *selected_id = 0;
                // Show empty state; list refresh comes via broadcast
                auto rp = std::static_pointer_cast<FlexColumn>(*right_pane);
                if (rp) {
                    rp->clearChildren();
                    auto lbl = *empty_label;
                    if (lbl) rp->addChild(lbl);
                }
            });
            hdr->addChild(del_btn);
        }

        // Save button
        auto save_btn = std::make_shared<ButtonWidget>("Save");
        save_btn->setFixedSize(60, 28);
        save_btn->setBackgroundColor(Color{0.18f, 0.48f, 0.88f, 1.0f});
        save_btn->onClick([ed_id, ed_title, ed_body, ed_pinned, ed_color,
                           selected_id, notes, rebuild_list, rebuild_editor]() {
            NoteEntry entry{};
            entry.id_      = *ed_id;
            entry.pinned_  = *ed_pinned ? 1 : 0;
            entry.color_   = *ed_color;
            std::strncpy(entry.title_, ed_title->c_str(), sizeof(entry.title_) - 1);
            std::strncpy(entry.body_,  ed_body->c_str(),  sizeof(entry.body_)  - 1);

            auto* xbus = XApplication::getBus();
            if (!xbus) return;
            auto opcode = (*ed_id == 0) ? NotesOpcode::Add : NotesOpcode::Update;
            xbus->Call("com.freebsdx.Notes",
                       static_cast<uint32_t>(opcode),
                       {reinterpret_cast<const uint8_t*>(&entry), sizeof(entry)},
                       [ed_id, selected_id](const XBusMessage& resp) {
                           NotesStatusResponse sr{};
                           std::memcpy(&sr, resp.payload_, sizeof(sr));
                           if (sr.success_) {
                               *ed_id      = sr.id_;
                               *selected_id = sr.id_;
                           }
                       });
        });
        hdr->addChild(save_btn);

        // ── Title input ───────────────────────────────────────────────
        auto title_input = std::make_shared<TextInputWidget>(*ed_title);
        title_input->setPlaceholder("Note title…");
        title_input->setFixedSize(0.0f, 44);
        title_input->setPadding(12);
        title_input->setBackgroundColor(Color{0.12f, 0.12f, 0.12f, 1.0f});
        title_input->setOnChanged([ed_title](const std::string& t) {
            *ed_title = t;
        });
        rp->addChild(title_input);

        // Thin color accent bar
        const auto& col = kColors[*ed_color];
        auto accent_bar = std::make_shared<FlexRow>();
        accent_bar->setFixedSize(0.0f, 3);
        accent_bar->setBackgroundColor(Color{col.r, col.g, col.b, 0.85f});
        rp->addChild(accent_bar);

        // ── Body input (multi-line) ───────────────────────────────────
        auto body_input = std::make_shared<MultilineTextInputWidget>(*ed_body);
        body_input->setPlaceholder("Start writing…");
        body_input->setPadding(14);
        body_input->setBackgroundColor(Color{0.10f, 0.10f, 0.10f, 1.0f});
        body_input->setOnChanged([ed_body](const std::string& t) {
            // Enforce 164-char protocol limit
            if (t.size() <= 164) {
                *ed_body = t;
            }
        });

        auto body_scroll = std::make_shared<ScrollViewWidget>(body_input);
        body_scroll->setFlexGrow(1.0f);
        rp->addChild(body_scroll);

        // ── Footer bar (timestamp + char count) ───────────────────────
        auto footer = std::make_shared<FlexRow>();
        footer->setFixedSize(0.0f, 28);
        footer->setPadding(8);
        footer->setGap(12);
        footer->setBackgroundColor(Color{0.12f, 0.12f, 0.12f, 1.0f});
        footer->setCrossAxisAlignment(CrossAxisAlignment::Center);
        rp->addChild(footer);

        auto char_count = std::make_shared<TextWidget>(
            std::to_string(ed_body->size()) + " / 164 chars", 11.0f);
        char_count->setColor(0.5f, 0.5f, 0.5f, 1.0f);
        footer->addChild(char_count);
    };

    // ══ OPEN NOTE LAMBDA ══════════════════════════════════════════════
    *open_note = [=](uint32_t id) {
        *selected_id = id;
        auto* xbus = XApplication::getBus();
        if (!xbus) return;

        NotesIdRequest req{};
        req.id_ = id;
        xbus->Call("com.freebsdx.Notes",
                   static_cast<uint32_t>(NotesOpcode::Get),
                   {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                   [ed_id, ed_title, ed_body, ed_pinned, ed_color,
                    rebuild_editor](const XBusMessage& resp) {
                       NotesListPage lp{};
                       std::memcpy(&lp, resp.payload_, sizeof(lp));
                       if (lp.count_ == 0) return;

                       LocalNote n = fromEntry(lp.entry_);
                       *ed_id     = n.id;
                       *ed_title  = n.title;
                       *ed_body   = n.body;
                       *ed_pinned = n.pinned;
                       *ed_color  = n.color;
                       if (*rebuild_editor) (*rebuild_editor)();
                   });
    };

    // ══ NEW NOTE LAMBDA ═══════════════════════════════════════════════
    *new_note = [=]() {
        *ed_id     = 0;
        *ed_title  = "";
        *ed_body   = "";
        *ed_pinned = false;
        *ed_color  = 0;
        if (*rebuild_editor) (*rebuild_editor)();
    };

    // ══ REBUILD LIST LAMBDA ═══════════════════════════════════════════
    *rebuild_list = [=]() {
        auto lc = std::static_pointer_cast<FlexColumn>(*list_col);
        if (!lc) return;
        lc->clearChildren();

        for (const auto& n : *notes) {
            auto row = std::make_shared<FlexRow>();
            row->setFixedSize(0.0f, 56);
            row->setPadding(8);
            row->setGap(8);
            row->setCrossAxisAlignment(CrossAxisAlignment::Center);

            // Highlight selected
            if (n.id == *selected_id)
                row->setBackgroundColor(Color{0.18f, 0.30f, 0.50f, 1.0f});
            else
                row->setBackgroundColor(Color{0.16f, 0.16f, 0.16f, 1.0f});

            // Color indicator dot
            const auto& col = kColors[n.color];
            auto dot = std::make_shared<FlexRow>();
            dot->setFixedSize(6, 36);
            dot->setBackgroundColor(Color{col.r, col.g, col.b, 0.9f});
            row->addChild(dot);

            // Text column: title + preview
            auto txt_col = std::make_shared<FlexColumn>();
            txt_col->setFlexGrow(1.0f);
            txt_col->setGap(3);
            row->addChild(txt_col);

            std::string title_str = n.pinned ? ("📌 " + n.title) : n.title;
            if (title_str.empty()) title_str = "(untitled)";
            auto title_w = std::make_shared<TextWidget>(title_str, 13.0f);
            title_w->setFontWeight(n.pinned ? 600 : 400);
            title_w->setColor(1.0f, 1.0f, 1.0f, 1.0f);
            txt_col->addChild(title_w);

            std::string preview = bodyPreview(n.body);
            if (!preview.empty()) {
                auto prev_w = std::make_shared<TextWidget>(preview, 11.0f);
                prev_w->setColor(0.6f, 0.6f, 0.6f, 1.0f);
                txt_col->addChild(prev_w);
            }

            uint32_t nid = n.id;
            auto note_btn = std::make_shared<ButtonWidget>(row, [nid, open_note]() {
                if (*open_note) (*open_note)(nid);
            });
            lc->addChild(note_btn);
        }

        if (notes->empty()) {
            auto empty = std::make_shared<TextWidget>(
                search_query->empty() ? "No notes yet" : "No results", 13.0f);
            empty->setColor(0.45f, 0.45f, 0.45f, 1.0f);
            empty->setPadding(12);
            lc->addChild(empty);
        }
    };

    // ══ LOAD PAGE LAMBDA ═══════════════════════════════════════════════
    *load_page = [=](uint32_t page) {
        auto* xbus = XApplication::getBus();
        if (!xbus) return;

        if (search_query->empty()) {
            NotesPageRequest req{};
            req.page_ = page;
            xbus->Call("com.freebsdx.Notes",
                       static_cast<uint32_t>(NotesOpcode::List),
                       {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                       [notes, rebuild_list, load_page](const XBusMessage& resp) {
                           NotesListPage lp{};
                           std::memcpy(&lp, resp.payload_, sizeof(lp));
                           if (lp.count_ > 0) notes->push_back(fromEntry(lp.entry_));
                           if (*rebuild_list) (*rebuild_list)();
                           if (lp.has_more_ && *load_page) (*load_page)(lp.page_ + 1);
                       });
        } else {
            NotesSearchRequest req{};
            req.page_ = page;
            std::strncpy(req.query_, search_query->c_str(), sizeof(req.query_) - 1);
            xbus->Call("com.freebsdx.Notes",
                       static_cast<uint32_t>(NotesOpcode::Search),
                       {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                       [notes, rebuild_list, load_page](const XBusMessage& resp) {
                           NotesListPage lp{};
                           std::memcpy(&lp, resp.payload_, sizeof(lp));
                           if (lp.count_ > 0) notes->push_back(fromEntry(lp.entry_));
                           if (*rebuild_list) (*rebuild_list)();
                           if (lp.has_more_ && *load_page) (*load_page)(lp.page_ + 1);
                       });
        }
    };

    return XApplication::Run(root_row, "/tmp/freebsdx.sock", false, false,
        [=](XBusClient& xbus) {
            XApplication::setTitle("Notes");

            // Broadcast listener
            xbus.OnBroadcast([=](const XBusMessage& bcast) {
                if (std::strcmp(bcast.service_name_, "com.freebsdx.Notes") == 0 &&
                    bcast.opcode_ == static_cast<uint32_t>(NotesOpcode::NoteChanged)) {
                    notes->clear();
                    if (*load_page) (*load_page)(0);
                }
            });

            // Initial load
            if (*load_page) (*load_page)(0);
        });
}
