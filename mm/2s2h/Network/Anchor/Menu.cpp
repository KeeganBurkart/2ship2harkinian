#ifdef ENABLE_ANCHOR

#include "Anchor.h"
#include "2s2h/BenGui/BenMenu.h"
#include "2s2h/BenGui/UIWidgets.hpp"
#include "2s2h/ShipUtils.h"
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <imgui.h>

namespace BenGui {
extern std::shared_ptr<BenMenu> mBenMenu;
}
using namespace BenGui;
using namespace UIWidgets;

static void SaveCVars() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

static void CVarTextInput(const char* label, const char* cvarName, const char* defaultValue, bool disabled) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s", CVarGetString(cvarName, defaultValue));
    ImGui::BeginDisabled(disabled);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    if (ImGui::InputText(label, buffer, sizeof(buffer))) {
        CVarSetString(cvarName, buffer);
        SaveCVars();
    }
    ImGui::EndDisabled();
}

void Anchor_DrawMenu() {
    Anchor* anchor = Anchor::Instance;
    bool enabled = anchor->isEnabled;

    ImGui::TextWrapped("Online co-op. Everyone in the same room sees each other; teammates share progress.");
    ImGui::Spacing();

    CVarTextInput("Your name", CVAR_ANCHOR("Name"), "", enabled);
    CVarTextInput("Room ID", CVAR_ANCHOR("RoomId"), "", enabled);
    CVarTextInput("Team ID", CVAR_ANCHOR("TeamId"), "default", enabled);

    if (ImGui::CollapsingHeader("Server")) {
        CVarTextInput("Host", CVAR_ANCHOR("Host"), ANCHOR_DEFAULT_HOST, enabled);
        int port = CVarGetInteger(CVAR_ANCHOR("Port"), ANCHOR_DEFAULT_PORT);
        ImGui::BeginDisabled(enabled);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::InputInt("Port", &port)) {
            CVarSetInteger(CVAR_ANCHOR("Port"), port);
            SaveCVars();
        }
        ImGui::EndDisabled();
    }

    bool missingInfo = Ship_IsCStringEmpty(CVarGetString(CVAR_ANCHOR("Name"), "")) ||
                       Ship_IsCStringEmpty(CVarGetString(CVAR_ANCHOR("RoomId"), ""));

    if (!enabled) {
        ImGui::BeginDisabled(missingInfo);
        if (Button("Connect", ButtonOptions().Size(Sizes::Inline))) {
            CVarSetInteger(CVAR_ANCHOR("Enabled"), 1);
            SaveCVars();
            anchor->Enable();
        }
        ImGui::EndDisabled();
        if (missingInfo) {
            ImGui::SameLine();
            ImGui::TextDisabled("Enter a name and room ID");
        }
        return;
    }

    if (Button("Disconnect", ButtonOptions().Size(Sizes::Inline))) {
        CVarSetInteger(CVAR_ANCHOR("Enabled"), 0);
        SaveCVars();
        anchor->Disable();
        return;
    }
    ImGui::SameLine();
    if (anchor->isConnected) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Connected");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Connecting...");
        return;
    }

    ImGui::SeparatorText("Players");
    if (ImGui::BeginTable("AnchorPlayers", 3, ImGuiTableFlags_SizingStretchProp)) {
        for (auto& [clientId, client] : anchor->clients) {
            ImGui::PushID(clientId);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", client.name.c_str(), client.self ? " (you)" : "");
            if (client.clientVersion != anchor->clientVersion) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(different build)");
            }
            ImGui::TableNextColumn();
            if (!client.online) {
                ImGui::TextDisabled("Offline");
            } else if (!client.isSaveLoaded) {
                ImGui::TextDisabled("In menus");
            } else {
                ImGui::TextUnformatted(Ship_GetSceneName(client.sceneId));
            }
            ImGui::TableNextColumn();
            if (anchor->CanTeleportTo(clientId)) {
                if (Button("Teleport", ButtonOptions().Size(Sizes::Inline))) {
                    anchor->SendPacket_RequestTeleport(clientId);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Progress");
    ImGui::BeginDisabled(!anchor->IsSyncing());
    if (Button("Sync progress from team", ButtonOptions().Size(Sizes::Inline))) {
        anchor->SendPacket_RequestTeamState();
    }
    ImGui::EndDisabled();

    if (anchor->roomState.ownerClientId == anchor->ownClientId) {
        ImGui::SeparatorText("Room settings (owner)");
        bool changed = false;
        changed |= CVarCheckbox("Share items and progress", CVAR_ANCHOR("RoomSettings.SyncItemsAndFlags"),
                                CheckboxOptions().DefaultValue(true));
        static const std::vector<const char*> teleportModes = { "Off", "Team only", "Anyone" };
        changed |= CVarCombobox("Teleport", CVAR_ANCHOR("RoomSettings.TeleportMode"), teleportModes,
                                ComboboxOptions().DefaultIndex(1));
        if (changed) {
            anchor->SendPacket_UpdateRoomState();
        }
    }
}

static void RegisterAnchorMenu() {
    mBenMenu->AddMenuEntry("Network", "gSettings.Menu.NetworkSidebarSection");
    mBenMenu->AddSidebarEntry("Network", "Co-op", 1);
    WidgetPath path = { "Network", "Co-op", SECTION_COLUMN_1 };
    mBenMenu->AddWidget(path, "Co-op", WIDGET_CUSTOM).CustomFunction([](WidgetInfo& info) { Anchor_DrawMenu(); });
}

static RegisterMenuInitFunc menuInitFunc(RegisterAnchorMenu);

#endif
