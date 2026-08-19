#pragma once

#include "app/editor.h"

#include <memory>
#include <vector>

namespace lime {

std::vector<std::unique_ptr<IPanel>> makeCorePanels();
std::vector<std::unique_ptr<IPanel>> makeScenePanels();
void drawEntityInspector(EditorContext& ed);
std::vector<std::unique_ptr<IPanel>> makeAssetPanels();
std::vector<std::unique_ptr<IPanel>> makeViewportPanels();
void viewportInit(void* d3d11Device, void* d3d11Context);
void viewportShutdown();
void viewportInvalidateMeshes();

bool drawAssetPicker(EditorContext& e, const char* label,
                     std::string_view assetType, const std::string& current,
                     std::string& out);

void canvasInit();
void canvasShutdown();
void releaseCanvas(GraphDoc& doc);

void drawGraphWindows(EditorContext& ed);
void drawTextDocument(EditorContext& ed);

void drawStatusBar(EditorContext& ed);
void drawNotes(EditorContext& ed);
void statusSetArea(const char* area);
void statusSetItemHover();
void statusInitLogo(void* d3d11Device);
void* createLogoTexture(void* d3d11Device);
void setGraphDockId(unsigned int dockId);
unsigned int graphDockId();

}
