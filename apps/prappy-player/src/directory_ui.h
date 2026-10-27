#pragma once
#include "directory_filter.h"
#include "feature_ui.h"
#include "preferences.h"
#include "compact_row.h"

struct DirectoryUi {
    std::wstring location;
    char search[128]{};
    uint64_t revision = UINT64_MAX;
    std::wstring shownLocation, shownSearch;
    std::vector<std::wstring> visible;
    bool overview = false, shownOverview = false;
    uint64_t choiceRevision = UINT64_MAX;
    std::vector<DirectoryFilter::Rule> lastIncluded, lastExcluded;
    std::vector<DirectoryIndex::Choice> choices;
    std::map<std::wstring,size_t> choicePositions;
    struct Change { bool filter = false, persistent = false; };
    template<class Observe, class Remember> Change draw(DirectoryIndex& index, DirectoryFilter& filter, Preferences& preferences, Observe observe, Remember remember) {
        Change change; bool openOverview = false;
        const auto hint = [](const std::string& value) { if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) { ImGui::BeginTooltip(); filenameText(value, ImGui::GetIO().DisplaySize.x - 32); ImGui::EndTooltip(); } };
        if (choiceRevision != index.revision || lastIncluded != filter.included || lastExcluded != filter.excluded) {
            choices = index.priorityChoices(filter); choicePositions.clear();
            for (size_t i = 0; i < choices.size(); ++i) choicePositions.emplace(choices[i].id,i);
            choiceRevision = index.revision; lastIncluded = filter.included; lastExcluded = filter.excluded;
            revision = UINT64_MAX;
        }
        const auto activeCount = filter.included.size()+filter.excluded.size();
        const auto onlyLabel = "Folders" + (activeCount ? " (" + std::to_string(activeCount) + ")" : std::string{});
        if (ImGui::Button((onlyLabel + "###directories").c_str())) { overview = false; ImGui::OpenPopup("Directory browser"); }
        observe("directories", ImGui::IsItemClicked()); remember(); hint("Choose included folders, hide subtrees, or pin priority folders");
        if (filter.active()) {
            ImGui::SameLine(); if (ImGui::Button("Reset##directories")) { filter.included.clear(); filter.excluded.clear(); change = {true, true}; }
            observe("directory-reset", ImGui::IsItemClicked()); remember();
        }
        const auto gap = ImGui::GetStyle().ItemSpacing.x, padding = ImGui::GetStyle().FramePadding.x;
        const auto available = std::max(0.0f,ImGui::GetWindowPos().x+ImGui::GetWindowContentRegionMax().x-ImGui::GetItemRectMax().x-gap);
        const auto labelFor = [&](const auto& node) {
            const bool excluded = DirectoryFilter::contains(filter.excluded,node.path), included = DirectoryFilter::contains(filter.included,node.path);
            return std::string(excluded ? "- " : included ? "+ " : "") + node.label + (node.count ? " ("+std::to_string(node.count)+")" : "");
        };
        const auto overflowLabel = [](size_t hidden) { return "+"+std::to_string(hidden)+" more"; };
        const auto layout = compactrow::plan(choices.size(),available,gap,ImGui::GetFontSize()*4+padding*2,ImGui::GetFontSize()*12+padding*2,
            [&](size_t i) { return text::active->width(labelFor(choices[i]),filenameFontSize())+padding*2; },
            [&](size_t hidden) { return ImGui::CalcTextSize(overflowLabel(hidden).c_str()).x+padding*2; });
        for (size_t shown = 0; shown < layout.widths.size(); ++shown) {
            const auto& node = choices[shown]; const bool selected = DirectoryFilter::contains(filter.included, node.path);
            const bool excluded = DirectoryFilter::contains(filter.excluded,node.path);
            const bool hidden = std::any_of(filter.excluded.begin(), filter.excluded.end(), [&](const auto& rule) { return inDirectory(node.id,rule.id); });
            ImGui::SameLine(); ImGui::PushID(utf8(node.path).c_str());
            if (selected || hidden) ImGui::PushStyleColor(ImGuiCol_Button, hidden ? ImVec4(.45f,.22f,.20f,1) : ImVec4(.16f,.43f,.34f,1));
            const auto label = labelFor(node); const auto width = layout.widths[shown];
            if (ImGui::Button("##folder-chip", {width, ImGui::GetFrameHeight()})) {
                if (excluded) { DirectoryFilter::set(filter.excluded,node.path,false); change = {true,true}; }
                else if (hidden && !selected) openOverview = true;
                else { DirectoryFilter::set(filter.included,node.path,!selected); change.filter = true; }
            }
            const auto min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->PushClipRect({min.x+5,min.y}, {min.x+width-5,min.y+ImGui::GetFrameHeight()}, true);
            highlightedLabel(label, {}, {min.x+padding,min.y+ImGui::GetStyle().FramePadding.y},width-padding*2);
            ImGui::GetWindowDrawList()->PopClipRect();
            observe(("directory-chip-" + std::to_string(shown)).c_str(), ImGui::IsItemClicked()); remember(); hint(utf8(node.path) + (excluded ? "\nHidden subtree. Click to restore it." : selected ? "\nIncluded subtree. Click to remove this selection." : hidden ? "\nHidden by a parent. Open the filter overview to restore it." : "\nClick to include; multiple selections are combined."));
            if (selected || hidden) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (openOverview) { overview = true; ImGui::OpenPopup("Directory browser"); }
        if (layout.hidden && layout.overflow > 0) {
            ImGui::SameLine(); if (ImGui::Button((overflowLabel(layout.hidden)+"###directory-overflow").c_str(),{layout.overflow,ImGui::GetFrameHeight()})) { overview = true; ImGui::OpenPopup("Directory browser"); }
            observe("directory-overflow",ImGui::IsItemClicked()); remember(); hint("Show remaining active filters, pinned folders and suggestions");
        }
        ImGui::SetNextWindowSizeConstraints({std::min(460.0f, ImGui::GetIO().DisplaySize.x-16), 180}, {std::max(300.0f, ImGui::GetIO().DisplaySize.x-16), std::min(520.0f,ImGui::GetIO().DisplaySize.y-16)});
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(.12f,.14f,.15f,1));
        if (ImGui::BeginPopup("Directory browser")) {
            if (!index.nodes.contains(location)) location.clear();
            ImGui::TextDisabled("Include any selected subtree. Hidden subtrees always win.");
            if (ImGui::SmallButton("All folders")) { filter.included.clear(); change.filter = true; }
            observe("directory-all", ImGui::IsItemClicked());
            ImGui::SameLine(); if (ImGui::SmallButton("Clear hidden")) { filter.excluded.clear(); change = {true,true}; }
            observe("directory-unhide-all", ImGui::IsItemClicked());
            if (overview) {
                ImGui::SameLine(); if (ImGui::SmallButton("Browse tree")) { overview = false; location.clear(); search[0] = '\0'; }
                observe("directory-browse-tree",ImGui::IsItemClicked());
            }
            if (!overview && !location.empty()) {
                if (ImGui::SmallButton("< Up")) {
                    const auto parent = directoryKey(index.nodes.at(location).path.parent_path());
                    location = index.nodes.contains(parent) && std::find(index.roots.begin(), index.roots.end(), location) == index.roots.end() ? parent : L"";
                    search[0] = '\0';
                }
                observe("directory-up", ImGui::IsItemClicked());
                if (!location.empty()) { ImGui::SameLine(); filenameText(index.nodes.at(location).label, 330); hint(utf8(index.nodes.at(location).path)); }
            }
            ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##directory-search", overview ? "Find priority folders..." : "Find in this level...", search, sizeof(search)); observe("directory-search", ImGui::IsItemClicked());
            const bool frameOverview = overview;
            const auto term = searchText(search);
            if (revision != index.revision || shownLocation != location || shownSearch != term || shownOverview != frameOverview) {
                visible.clear();
                if (frameOverview) { for (const auto& choice : choices) if (searchText(utf8(choice.path)).find(term) != std::wstring::npos) visible.push_back(choice.id); }
                else { const auto& candidates = location.empty() ? index.roots : index.nodes.at(location).children;
                    for (const auto& id : candidates) if (searchText(index.nodes.at(id).label).find(term) != std::wstring::npos) visible.push_back(id); }
                revision = index.revision; shownLocation = location; shownSearch = term; shownOverview = frameOverview;
            }
            if (ImGui::BeginTable("Directory level", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, {0,std::clamp(ImGui::GetIO().DisplaySize.y-190,80.0f,260.0f)})) {
                ImGui::TableSetupColumn("Include", ImGuiTableColumnFlags_WidthFixed, 48);
                ImGui::TableSetupColumn("Hide", ImGuiTableColumnFlags_WidthFixed, 34);
                ImGui::TableSetupColumn("Pin", ImGuiTableColumnFlags_WidthFixed, 28);
                ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 28); ImGui::TableHeadersRow();
                ImGuiListClipper clipper; clipper.Begin(static_cast<int>(visible.size()));
                while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto& id = visible[static_cast<size_t>(i)];
                    DirectoryIndex::Choice node;
                    if (frameOverview) node = choices[choicePositions.at(id)];
                    else { const auto& source = index.nodes.at(id); node = {source.path,id,source.label,source.count,!source.children.empty()}; }
                    ImGui::PushID(utf8(node.path).c_str()); ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); bool included = DirectoryFilter::contains(filter.included, node.path);
                    if (ImGui::Checkbox("##include", &included)) { DirectoryFilter::set(filter.included, node.path, included); change.filter = true; }
                    observe(("directory-include-"+std::to_string(i)).c_str(), ImGui::IsItemClicked());
                    ImGui::TableSetColumnIndex(1); bool hidden = DirectoryFilter::contains(filter.excluded, node.path);
                    if (ImGui::Checkbox("##hide", &hidden)) { DirectoryFilter::set(filter.excluded, node.path, hidden); change = {true,true}; }
                    observe(("directory-hide-"+std::to_string(i)).c_str(), ImGui::IsItemClicked());
                    ImGui::TableSetColumnIndex(2); bool pinned = std::any_of(preferences.pinnedDirectories.begin(), preferences.pinnedDirectories.end(), [&](const auto& path) { return directoryKey(path) == node.id; });
                    if (ImGui::Checkbox("##pin", &pinned)) {
                        std::erase_if(preferences.pinnedDirectories, [&](const auto& path) { return directoryKey(path) == node.id; });
                        if (pinned) preferences.pinnedDirectories.push_back(node.path);
                        index.prioritize(preferences.pinnedDirectories); change.persistent = true;
                    }
                    observe(("directory-pin-"+std::to_string(i)).c_str(), ImGui::IsItemClicked());
                    ImGui::TableSetColumnIndex(3);
                    const auto breadcrumb = node.path.filename().empty() ? node.path : node.path.parent_path().filename()/node.path.filename();
                    const auto rowLabel = (frameOverview ? utf8(breadcrumb) : node.label) + (node.count ? " ("+std::to_string(node.count)+")" : "");
                    const auto textWidth = ImGui::GetContentRegionAvail().x;
                    const auto origin = ImGui::GetCursorScreenPos(); ImGui::Dummy({textWidth,ImGui::GetFontSize()});
                    highlightedLabel(rowLabel,{},origin,textWidth); hint(utf8(node.path));
                    ImGui::TableSetColumnIndex(4);
                    if (node.hasChildren) {
                        if (ImGui::SmallButton(">")) { location = node.id; overview = false; search[0] = '\0'; }
                        observe(("directory-open-"+std::to_string(i)).c_str(), ImGui::IsItemClicked());
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (visible.empty()) ImGui::TextDisabled("No subdirectories at this level.");
            ImGui::TextDisabled("Includes are temporary; hidden folders and pins are remembered.");
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        return change;
    }
};
