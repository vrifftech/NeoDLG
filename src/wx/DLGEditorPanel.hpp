#pragma once
#include "NeoModulePanel.hpp"
#include <neoshared/ResourceDocument.hpp>
#include <gff/AppModel.hpp>
namespace neodlg::ui {
inline constexpr unsigned kEditorApiVersion=1;
class DLGEditorPanel : public neomodules::Panel {
public:
    using Panel::Panel;
    virtual bool openFile(const std::filesystem::path& path)=0;
    virtual bool openResource(neoshared::ResourceDocument resource)=0;
    virtual bool saveActiveAs(const std::filesystem::path& path)=0;
    virtual bool activateResource(const std::string& identity)=0;
    virtual std::size_t documentCount() const=0;
    virtual neogff::GffModel* activeModel()=0;
    virtual void refreshActiveDocument()=0;
};
DLGEditorPanel* createEditorPanel(wxWindow* parent, neomodules::Context context={});
} // namespace neodlg::ui
