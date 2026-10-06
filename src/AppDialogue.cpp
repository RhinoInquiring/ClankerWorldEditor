// NPC dialogue: gossip menus (gossip_menu + npc_text + gossip_menu_option), their conditions, and creature_text barks,
// edited in the NPC viewer's Dialogue tab and applied with the NPC's other edits.
#include "App.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdlib>

namespace
{
const ImVec4 kQuiet{ 0.60f, 0.62f, 0.66f, 1.00f };
const ImVec4 kWarn{ 1.00f, 0.66f, 0.25f, 1.00f };

uint32_t U(const std::string& s) { return uint32_t(std::strtoul(s.c_str(), nullptr, 10)); }
float F(const std::string& s) { return std::strtof(s.c_str(), nullptr); }
std::string Col(const nlohmann::json& row, const char* col)
{
    if (!row.is_object()) return {};
    const auto it = row.find(col);
    return it != row.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// conditions.SourceTypeOrReferenceId of a menu's texts and of its options.
constexpr int kSourceText = 14, kSourceOption = 15;

/// Option icons (GOSSIP_ICON_*) and the client's picture for each (GossipFrame.lua: Interface\GossipFrame\<type>GossipIcon).
struct Icon { const char* name; const char* texture; };
const Icon kIcons[] = { { "Chat", "Gossip" },     { "Vendor", "Vendor" },   { "Taxi", "Taxi" },     { "Trainer", "Trainer" }, { "Interact", "Healer" },
                        { "Interact 2", "Binder" }, { "Money bag", "Banker" }, { "Talk", "Petition" }, { "Tabard", "Tabard" }, { "Battle", "BattleMaster" },
                        { "Dot", "Gossip" } };
std::string IconTexture(uint32_t icon) { return std::string("Interface\\GossipFrame\\") + (icon < std::size(kIcons) ? kIcons[icon].texture : "Gossip") + "GossipIcon.blp"; }

/// gossip_menu_option.OptionType (GossipOption in AzerothCore GossipDef.h) and the npcflag the option needs.
struct OptionType { const char* name; uint32_t npcflag; };
const OptionType kOptionTypes[] = { { "None (script)", 0 },      { "Gossip (submenu)", 1 },  { "Quest giver", 2 },        { "Vendor", 128 },
                                    { "Flight master", 8192 },  { "Trainer", 16 },          { "Spirit healer", 16384 },  { "Spirit guide", 32768 },
                                    { "Innkeeper", 65536 },      { "Banker", 131072 },       { "Petitioner", 262144 },    { "Tabard designer", 524288 },
                                    { "Battlemaster", 1048576 }, { "Auctioneer", 2097152 },  { "Stable master", 4194304 }, { "Repair", 4096 },
                                    { "Unlearn talents", 16 },   { "Unlearn pet talents", 16 }, { "Learn dual spec", 16 }, { "Outdoor PvP", 0 },
                                    { "Dual spec info", 16 } };

/// The condition types worth offering by name (ConditionTypes in AzerothCore ConditionMgr.h) and what their values mean.
struct ConditionType { int id; const char* name; const char* values[3]; };
const ConditionType kConditionTypes[] = {
    { 8, "Quest rewarded", { "quest", nullptr, nullptr } },        { 9, "Quest taken", { "quest", nullptr, nullptr } },
    { 28, "Quest complete", { "quest", nullptr, nullptr } },        { 14, "Quest not taken", { "quest", nullptr, nullptr } },
    { 47, "Quest state", { "quest", "state mask", nullptr } },      { 43, "Daily quest done", { "quest", nullptr, nullptr } },
    { 27, "Level", { "level", "0 = 1 > 2 < 3 >= 4 <=", nullptr } }, { 15, "Class", { "class mask", nullptr, nullptr } },
    { 16, "Race", { "race mask", nullptr, nullptr } },              { 20, "Gender", { "0 male, 1 female", nullptr, nullptr } },
    { 6, "Team", { "469 Alliance, 67 Horde", nullptr, nullptr } },  { 5, "Reputation rank", { "faction", "rank mask", nullptr } },
    { 2, "Has item", { "item", "count", "bank too" } },             { 3, "Item equipped", { "item", nullptr, nullptr } },
    { 1, "Has aura", { "spell", "effect index", nullptr } },        { 25, "Knows spell", { "spell", nullptr, nullptr } },
    { 7, "Skill", { "skill", "value", nullptr } },                  { 17, "Achievement", { "achievement", nullptr, nullptr } },
    { 18, "Title", { "title", nullptr, nullptr } },                 { 12, "Game event active", { "event", nullptr, nullptr } },
    { 4, "In zone", { "zone", nullptr, nullptr } },                 { 23, "In area", { "area", nullptr, nullptr } },
    { 22, "On map", { "map", nullptr, nullptr } },
};
const ConditionType* FindCondition(int id)
{
    for (const ConditionType& t : kConditionTypes)
        if (t.id == id) return &t;
    return nullptr;
}

const char* const kBarkTypes[] = { "Say", "Yell", "Emote", "Boss emote", "Whisper", "Boss whisper" };
const int kBarkTypeIds[] = { 12, 14, 16, 41, 15, 42 };
const char* const kRanges[] = { "Normal", "Area", "Zone", "Map", "World" };

/// The text a menu shows: its first text's first non-empty line (the one most players see).
std::string FirstLine(const nlohmann::json& text)
{
    for (int g = 0; g < 8; ++g)
        for (int s = 0; s < 2; ++s)
            if (std::string t = Col(text, ("text" + std::to_string(g) + "_" + std::to_string(s)).c_str()); !t.empty()) return t;
    return {};
}
}

void App::LoadGossipMenu(uint32_t menu)
{
    NpcView::Dialogue& d = m_npc.dialogue;
    if (!menu || d.menus.count(menu)) return;
    d.menus[menu] = m_gossipMenus.Rows(menu);
    d.options[menu] = m_gossipOptions.Rows(menu);
    d.conditions[menu] = m_gossipConditions.Rows(menu);
    for (const auto& r : d.menus[menu])
        if (const uint32_t t = U(Col(r, "TextID")); t && !d.texts.count(t))
        {
            const auto rows = m_npcTexts.Rows(t);
            d.texts[t] = rows.empty() ? nlohmann::json() : rows[0];
        }
}

uint32_t App::NewDialogueId(TableRowsAdapter& table, const char* kind, const std::vector<uint32_t>& buffered)
{
    if (!m_project) return 0;
    const Project::IdRange r = m_project->Range(kind);
    uint32_t id = table.NextKey(r.first, r.last).value_or(0);
    for (uint32_t b : buffered)   // ids handed out in this edit, not written yet
        if (b >= r.first && b <= r.last && b >= id) id = b + 1;
    if (!id || id > r.last) { Log("No free %s in the project's range %u-%u (File > Project settings).", kind, r.first, r.last); return 0; }
    return id;
}

uint32_t App::NewNpcText(const std::string& text)
{
    NpcView::Dialogue& d = m_npc.dialogue;
    std::vector<uint32_t> ids;
    for (const auto& [id, row] : d.texts) ids.push_back(id);
    const uint32_t id = NewDialogueId(m_npcTexts, "npc_text.id", ids);
    if (!id) return 0;
    // Every column of npc_text (eight text groups with language, chance and emotes): texts empty, numbers 0.
    nlohmann::json row = nlohmann::json::object();
    std::string error;
    if (const auto cols = m_db.Query("SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'npc_text'", error))
        for (const auto& c : *cols) row[c[0]] = c[0].rfind("text", 0) == 0 ? "" : "0";
    row["ID"] = std::to_string(id);
    row["text0_0"] = text;
    row["Probability0"] = "1";
    d.texts[id] = row;
    return id;
}

uint32_t App::NewGossipMenu(const std::string& text)
{
    NpcView::Dialogue& d = m_npc.dialogue;
    std::vector<uint32_t> ids;
    for (const auto& [id, rows] : d.menus) ids.push_back(id);
    const uint32_t menu = NewDialogueId(m_gossipMenus, "gossip_menu.id", ids);
    const uint32_t textId = menu ? NewNpcText(text) : 0;
    if (!textId) return 0;
    d.menus[menu] = { { { "MenuID", std::to_string(menu) }, { "TextID", std::to_string(textId) } } };
    d.options[menu] = {};
    d.conditions[menu] = {};
    m_npc.dirty = true;
    return menu;
}

void App::DialogueChanges(std::vector<Change>& parts, const std::string& label, std::set<std::string>& reloads)
{
    NpcView::Dialogue& d = m_npc.dialogue;
    auto put = [&](TableRowsAdapter& table, uint32_t key, const std::vector<nlohmann::json>& after) {
        const auto before = table.Rows(key);
        if (before == after) return;
        Change c = table.MakeChange(key, before, after, label);
        table.Apply(c);
        if (!table.LastError().empty()) Log("%s", table.LastError().c_str());
        parts.push_back(std::move(c));
        reloads.insert(table.Table());
    };
    for (const auto& [menu, rows] : d.menus) put(m_gossipMenus, menu, rows);
    for (const auto& [menu, rows] : d.options) put(m_gossipOptions, menu, rows);
    for (const auto& [menu, rows] : d.conditions) put(m_gossipConditions, menu, rows);
    for (const auto& [id, row] : d.texts)
        if (row.is_object()) put(m_npcTexts, id, { row });
    if (d.barksLoaded) put(m_creatureTexts, m_npc.entry, d.barks);
}

void App::DrawConditions(uint32_t menu, int source, uint32_t entry)
{
    NpcView& v = m_npc;
    std::vector<nlohmann::json>& rows = v.dialogue.conditions[menu];
    const std::string src = std::to_string(source), ent = std::to_string(entry);
    std::optional<size_t> remove;
    int shown = 0;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        nlohmann::json& c = rows[i];
        if (Col(c, "SourceTypeOrReferenceId") != src || Col(c, "SourceEntry") != ent) continue;
        ++shown;
        ImGui::PushID(int(i) + 5000);
        const int type = std::atoi(Col(c, "ConditionTypeOrReference").c_str());
        const ConditionType* known = FindCondition(type);
        ImGui::SetNextItemWidth(150);
        if (ImGui::BeginCombo("##type", known ? known->name : ("Type " + std::to_string(type)).c_str(), ImGuiComboFlags_HeightLarge))
        {
            for (const ConditionType& t : kConditionTypes)
                if (ImGui::Selectable(t.name, t.id == type)) { c["ConditionTypeOrReference"] = std::to_string(t.id); v.dirty = true; }
            ImGui::EndCombo();
        }
        for (int k = 0; k < 3; ++k)
        {
            const char* meaning = known ? known->values[k] : "value";
            if (!meaning) continue;
            ImGui::SameLine();
            const std::string col = "ConditionValue" + std::to_string(k + 1);
            int x = int(U(Col(c, col.c_str())));
            ImGui::SetNextItemWidth(80);
            if (ImGui::InputInt(("##v" + std::to_string(k)).c_str(), &x, 0)) { c[col] = std::to_string(std::max(x, 0)); v.dirty = true; }
            ImGui::SetItemTooltip("%s", meaning);
        }
        ImGui::SameLine();
        if (bool negate = Col(c, "NegativeCondition") == "1"; ImGui::Checkbox("not", &negate)) { c["NegativeCondition"] = negate ? "1" : "0"; v.dirty = true; }
        ImGui::SameLine();
        int group = int(U(Col(c, "ElseGroup")));
        ImGui::SetNextItemWidth(40);
        if (ImGui::InputInt("##else", &group, 0)) { c["ElseGroup"] = std::to_string(std::max(group, 0)); v.dirty = true; }
        ImGui::SetItemTooltip("Else group: conditions of one group must all hold; any one group is enough");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = i;
        ImGui::PopID();
    }
    if (remove) { rows.erase(rows.begin() + std::ptrdiff_t(*remove)); v.dirty = true; }
    if (ImGui::SmallButton(shown ? "+ condition" : "+ show only when..."))
    {
        rows.push_back({ { "SourceTypeOrReferenceId", src }, { "SourceGroup", std::to_string(menu) }, { "SourceEntry", ent }, { "SourceId", "0" },
                         { "ElseGroup", "0" }, { "ConditionTypeOrReference", "9" }, { "ConditionTarget", "0" }, { "ConditionValue1", "0" },
                         { "ConditionValue2", "0" }, { "ConditionValue3", "0" }, { "NegativeCondition", "0" }, { "ErrorType", "0" }, { "ErrorTextId", "0" },
                         { "ScriptName", "" }, { "Comment", Col(v.editTemplate, "name") + " - gossip" } });
        v.dirty = true;
    }
}

void App::DrawNpcDialogueTab()
{
    NpcView& v = m_npc;
    if (!v.entry || !v.editTemplate.is_object()) { ImGui::TextColored(kQuiet, "No creature open."); return; }
    if (ImGui::BeginTabBar("##dialogue"))
    {
        if (ImGui::BeginTabItem("Gossip")) { DrawGossip(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Barks")) { DrawBarks(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

void App::DrawGossip()
{
    NpcView& v = m_npc;
    NpcView::Dialogue& d = v.dialogue;
    const uint32_t root = U(Col(v.editTemplate, "gossip_menu_id"));
    if (!root)
    {
        ImGui::TextWrapped("No gossip menu: talking to this creature shows nothing (or its quests and services only).");
        if (ImGui::Button("Create a gossip menu"))
            if (const uint32_t menu = NewGossipMenu("Greetings, $N."))
            {
                v.editTemplate["gossip_menu_id"] = std::to_string(menu);
                v.editTemplate["npcflag"] = std::to_string(U(Col(v.editTemplate, "npcflag")) | 1);
                d.current = menu;
                d.trail.clear();
            }
        return;
    }
    if (!(U(Col(v.editTemplate, "npcflag")) & 1))
    {
        ImGui::TextColored(kWarn, "The Gossip NPC flag is off: players cannot open this menu.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Turn it on")) { v.editTemplate["npcflag"] = std::to_string(U(Col(v.editTemplate, "npcflag")) | 1); v.dirty = true; }
    }
    if (!d.current || (d.trail.empty() && d.current != root)) d.current = root;
    LoadGossipMenu(d.current);
    const uint32_t menu = d.current;

    // Where we are: the root menu, then each submenu followed.
    if (ImGui::SmallButton(("Menu " + std::to_string(root)).c_str())) { d.current = root; d.trail.clear(); }
    for (size_t i = 0; i < d.trail.size(); ++i)
    {
        ImGui::SameLine();
        ImGui::TextColored(kQuiet, ">");
        ImGui::SameLine();
        if (ImGui::SmallButton(("Menu " + std::to_string(d.trail[i]) + "##t" + std::to_string(i)).c_str()))
        {
            d.current = d.trail[i];
            d.trail.resize(i + 1);
        }
    }
    std::string error;
    if (const auto users = m_db.Query("SELECT COUNT(*) FROM creature_template WHERE gossip_menu_id = " + std::to_string(menu) + " AND entry <> " + std::to_string(v.entry), error);
        users && !users->empty() && std::atoi((*users)[0][0].c_str()) > 0)
        ImGui::TextColored(kWarn, "Menu %u is also used by %s other creature(s): edits change theirs too.", menu, (*users)[0][0].c_str());

    // Preview, as the game's gossip window lays it out: the text, then the options with their icons.
    auto& texts = d.menus[menu];
    auto& options = d.options[menu];
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.93f, 0.86f, 0.70f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.18f, 0.12f, 0.05f, 1));
    if (ImGui::BeginChild("##preview", { 0, 190 }, ImGuiChildFlags_Borders))
    {
        // What most players see: the last text without conditions (the server shows the last text whose conditions hold).
        uint32_t firstText = 0;
        for (const auto& t : texts)
        {
            const std::string id = Col(t, "TextID");
            const bool conditional = std::any_of(d.conditions[menu].begin(), d.conditions[menu].end(),
                                                 [&](const nlohmann::json& c) { return Col(c, "SourceTypeOrReferenceId") == "14" && Col(c, "SourceEntry") == id; });
            if (!conditional || !firstText) firstText = U(id);
        }
        std::string body = firstText && d.texts.count(firstText) ? FirstLine(d.texts[firstText]) : std::string();
        for (size_t p; (p = body.find("$B")) != std::string::npos || (p = body.find("$b")) != std::string::npos;) body.replace(p, 2, "\n");
        ImGui::TextWrapped("%s", body.empty() ? "(no text)" : body.c_str());
        ImGui::Spacing();
        for (const auto& o : options)
        {
            const uint32_t icon = U(Col(o, "OptionIcon"));
            if (ID3D11ShaderResourceView* tex = m_renderer.TextureFor(IconTexture(icon), m_mpq)) ImGui::Image(ImTextureID(intptr_t(tex)), { 16, 16 });
            ImGui::SameLine();
            ImGui::PushID(&o);
            const uint32_t target = U(Col(o, "ActionMenuID"));
            if (ImGui::Selectable(Col(o, "OptionText").c_str(), false, 0, { 0, 0 }) && target)
            {
                d.trail.push_back(target);
                d.current = target;
            }
            if (target) ImGui::SetItemTooltip("Opens menu %u", target);
            ImGui::PopID();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.28f, 0.18f, 1));
        ImGui::TextWrapped("$N name  $C class  $R race  $B new line   (click an option that opens a menu to follow it)");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    if (d.current != menu) return;   // followed an option: draw the new menu next frame

    // Texts: the server shows the last (by id) whose conditions hold.
    ImGui::SeparatorText("Text");
    std::optional<size_t> removeText;
    for (size_t i = 0; i < texts.size(); ++i)
    {
        const uint32_t id = U(Col(texts[i], "TextID"));
        ImGui::PushID(int(i));
        nlohmann::json& t = d.texts[id];
        ImGui::TextColored(kQuiet, "npc_text %u%s", id, m_project && m_project->Owns("npc_text.id", id) ? "" : "  (stock: other menus may show it)");
        if (texts.size() > 1)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("remove")) removeText = i;
        }
        if (t.is_object())
        {
            std::string male = Col(t, "text0_0"), female = Col(t, "text0_1");
            if (ImGui::InputTextMultiline("##male", &male, { -1, 60 })) { t["text0_0"] = male; v.dirty = true; }
            ImGui::SetItemTooltip("The text (to men, and to everyone when the second one is empty)");
            if (ImGui::InputTextMultiline("##female", &female, { -1, 36 })) { t["text0_1"] = female; v.dirty = true; }
            ImGui::SetItemTooltip("To women (optional)");
            if (!Col(t, "BroadcastTextID0").empty() && Col(t, "BroadcastTextID0") != "0")
                ImGui::TextColored(kWarn, "Uses BroadcastTextID %s: the client may show that text instead.", Col(t, "BroadcastTextID0").c_str());
        }
        else ImGui::TextColored(kWarn, "npc_text %u does not exist.", id);
        if (texts.size() > 1 || std::any_of(d.conditions[menu].begin(), d.conditions[menu].end(), [&](const nlohmann::json& c) { return Col(c, "SourceEntry") == std::to_string(id) && Col(c, "SourceTypeOrReferenceId") == "14"; }))
            DrawConditions(menu, kSourceText, id);
        ImGui::PopID();
        ImGui::Spacing();
    }
    if (removeText) { texts.erase(texts.begin() + std::ptrdiff_t(*removeText)); v.dirty = true; }
    if (ImGui::SmallButton("+ another text (replaces the ones above while its conditions hold)"))
        if (const uint32_t id = NewNpcText("..."))
        {
            texts.push_back({ { "MenuID", std::to_string(menu) }, { "TextID", std::to_string(id) } });   // a higher id: checked after, so it wins
            v.dirty = true;
        }
    if (texts.size() > 1) ImGui::TextColored(kQuiet, "Several texts: the last one whose conditions hold is shown.");

    // Options.
    ImGui::SeparatorText("Options");
    std::optional<size_t> removeOption;
    for (size_t i = 0; i < options.size(); ++i)
    {
        nlohmann::json& o = options[i];
        ImGui::PushID(int(i) + 1000);
        const uint32_t icon = U(Col(o, "OptionIcon")), type = U(Col(o, "OptionType")), option = U(Col(o, "OptionID"));
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("##icon", icon < std::size(kIcons) ? kIcons[icon].name : std::to_string(icon).c_str()))
        {
            for (uint32_t k = 0; k < std::size(kIcons); ++k)
            {
                if (ID3D11ShaderResourceView* tex = m_renderer.TextureFor(IconTexture(k), m_mpq)) ImGui::Image(ImTextureID(intptr_t(tex)), { 16, 16 });
                ImGui::SameLine();
                if (ImGui::Selectable(kIcons[k].name, k == icon)) { o["OptionIcon"] = std::to_string(k); v.dirty = true; }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        std::string text = Col(o, "OptionText");
        ImGui::SetNextItemWidth(-60);
        if (ImGui::InputText("##text", &text)) { o["OptionText"] = text; v.dirty = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeOption = i;
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("##type", type < std::size(kOptionTypes) ? kOptionTypes[type].name : std::to_string(type).c_str(), ImGuiComboFlags_HeightLarge))
        {
            for (uint32_t k = 0; k < std::size(kOptionTypes); ++k)
                if (ImGui::Selectable(kOptionTypes[k].name, k == type))
                {
                    o["OptionType"] = std::to_string(k);
                    o["OptionNpcFlag"] = std::to_string(kOptionTypes[k].npcflag);   // the flag the server checks the creature for
                    v.dirty = true;
                }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("What choosing it does; the creature needs the matching NPC flag");
        if (const uint32_t need = U(Col(o, "OptionNpcFlag")); need && (U(Col(v.editTemplate, "npcflag")) & need) != need)
        {
            ImGui::SameLine();
            ImGui::TextColored(kWarn, "NPC flag missing");
            ImGui::SameLine();
            if (ImGui::SmallButton("add")) { v.editTemplate["npcflag"] = std::to_string(U(Col(v.editTemplate, "npcflag")) | need); v.dirty = true; }
        }
        // Submenu: choosing the option opens another menu.
        const uint32_t target = U(Col(o, "ActionMenuID"));
        ImGui::SameLine();
        if (target)
        {
            if (ImGui::SmallButton(("Opens menu " + std::to_string(target) + " >").c_str())) { d.trail.push_back(target); d.current = target; }
            ImGui::SameLine();
            if (ImGui::SmallButton("unlink")) { o["ActionMenuID"] = "0"; v.dirty = true; }
        }
        else if (ImGui::SmallButton("+ submenu"))
            if (const uint32_t sub = NewGossipMenu("..."))
            {
                o["ActionMenuID"] = std::to_string(sub);
                if (type == 0) { o["OptionType"] = "1"; o["OptionNpcFlag"] = "1"; }
            }
        // Confirmation box (with a price) before the option runs.
        bool box = !Col(o, "BoxText").empty();
        if (ImGui::Checkbox("Ask first", &box)) { o["BoxText"] = box ? "Are you sure?" : ""; v.dirty = true; }
        if (box)
        {
            ImGui::SameLine();
            std::string boxText = Col(o, "BoxText");
            ImGui::SetNextItemWidth(-130);
            if (ImGui::InputText("##box", &boxText)) { o["BoxText"] = boxText; v.dirty = true; }
            ImGui::SameLine();
            int money = int(U(Col(o, "BoxMoney")));
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputInt("##money", &money, 0)) { o["BoxMoney"] = std::to_string(std::max(money, 0)); v.dirty = true; }
            ImGui::SetItemTooltip("Price in copper (0: free)");
        }
        DrawConditions(menu, kSourceOption, option);
        ImGui::PopID();
        ImGui::Separator();
    }
    if (removeOption) { options.erase(options.begin() + std::ptrdiff_t(*removeOption)); v.dirty = true; }
    if (ImGui::Button("Add option"))
    {
        uint32_t next = 0;
        for (const auto& o : options) next = std::max(next, U(Col(o, "OptionID")) + 1);
        options.push_back({ { "MenuID", std::to_string(menu) }, { "OptionID", std::to_string(next) }, { "OptionIcon", "0" }, { "OptionText", "Tell me more." },
                            { "OptionBroadcastTextID", "0" }, { "OptionType", "1" }, { "OptionNpcFlag", "1" }, { "ActionMenuID", "0" }, { "ActionPoiID", "0" },
                            { "BoxCoded", "0" }, { "BoxMoney", "0" }, { "BoxText", "" }, { "BoxBroadcastTextID", "0" }, { "VerifiedBuild", "0" } });
        v.dirty = true;
    }
    ImGui::TextColored(kQuiet, "npc_text is read when worldserver starts (no reload); menus, options and conditions reload on Apply.");
}

void App::DrawBarks()
{
    NpcView& v = m_npc;
    NpcView::Dialogue& d = v.dialogue;
    if (!d.barksLoaded)
    {
        d.barks = m_creatureTexts.Rows(v.entry);
        d.barksLoaded = true;
    }
    ImGui::TextWrapped("What it says, yells or emotes (creature_text). Scripts say a group: SmartAI's Talk action or C++ Talk(group); "
                       "one line of the group is picked by chance.");
    std::optional<size_t> remove;
    if (ImGui::BeginTable("##barks", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Range", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Chance", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 18);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < d.barks.size(); ++i)
        {
            nlohmann::json& b = d.barks[i];
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (int g = int(U(Col(b, "GroupID"))); ImGui::InputInt("##g", &g, 0)) { b["GroupID"] = std::to_string(std::clamp(g, 0, 255)); v.dirty = true; }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (std::string t = Col(b, "Text"); ImGui::InputText("##t", &t)) { b["Text"] = t; v.dirty = true; }
            ImGui::TableNextColumn();
            const int type = std::atoi(Col(b, "Type").c_str());
            const char* typeName = "Type";
            for (size_t k = 0; k < std::size(kBarkTypeIds); ++k)
                if (kBarkTypeIds[k] == type) typeName = kBarkTypes[k];
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##ty", typeName))
            {
                for (size_t k = 0; k < std::size(kBarkTypeIds); ++k)
                    if (ImGui::Selectable(kBarkTypes[k], kBarkTypeIds[k] == type)) { b["Type"] = std::to_string(kBarkTypeIds[k]); v.dirty = true; }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            const uint32_t range = U(Col(b, "TextRange"));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##r", range < std::size(kRanges) ? kRanges[range] : "?"))
            {
                for (uint32_t k = 0; k < std::size(kRanges); ++k)
                    if (ImGui::Selectable(kRanges[k], k == range)) { b["TextRange"] = std::to_string(k); v.dirty = true; }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (float p = F(Col(b, "Probability")); ImGui::DragFloat("##p", &p, 1, 0, 100, "%.0f", ImGuiSliderFlags_AlwaysClamp))
            {
                char text[16];
                snprintf(text, sizeof text, "%g", p);
                b["Probability"] = text;
                v.dirty = true;
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) remove = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove) { d.barks.erase(d.barks.begin() + std::ptrdiff_t(*remove)); v.dirty = true; }
    if (d.barks.empty()) ImGui::TextColored(kQuiet, "No lines yet.");
    if (ImGui::Button("Add line"))
    {
        uint32_t group = 0, id = 0;
        for (const auto& b : d.barks) group = std::max(group, U(Col(b, "GroupID")));
        for (const auto& b : d.barks)
            if (U(Col(b, "GroupID")) == group) id = std::max(id, U(Col(b, "ID")) + 1);
        d.barks.push_back({ { "CreatureID", std::to_string(v.entry) }, { "GroupID", std::to_string(group) }, { "ID", std::to_string(id) },
                            { "Text", "..." }, { "Type", "12" }, { "Language", "0" }, { "Probability", "100" }, { "Emote", "0" }, { "Duration", "0" },
                            { "Sound", "0" }, { "BroadcastTextId", "0" }, { "TextRange", "0" }, { "comment", Col(v.editTemplate, "name") } });
        v.dirty = true;
    }
}
