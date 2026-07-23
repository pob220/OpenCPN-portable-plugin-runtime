#include "surface_dialog.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace ppm {
namespace {

wxString Text(const std::string& value) {
  return wxString::FromUTF8(value);
}

std::string JsonString(const wxString& value) {
  const wxScopedCharBuffer bytes = value.utf8_str();
  const std::string input =
      bytes ? std::string(bytes.data(), bytes.length()) : std::string();
  std::string output = "\"";
  output.reserve(input.size() + 2);
  for (const unsigned char character : input) {
    switch (character) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (character < 0x20) {
          static const char digits[] = "0123456789abcdef";
          output += "\\u00";
          output += digits[character >> 4];
          output += digits[character & 0x0f];
        } else {
          output += static_cast<char>(character);
        }
    }
  }
  output += '"';
  return output;
}

bool IsButton(const std::string& type) {
  return type == "button" || type == "cancel" ||
         type == "file-open" || type == "file-open-multiple" ||
         type == "file-save" || type == "navigation-create";
}

}  // namespace

SurfaceDialog::SurfaceDialog(wxWindow* parent,
                             DeclarativeSurface definition,
                             EventCallback callback)
    : wxFrame(parent, wxID_ANY, Text(definition.title), wxDefaultPosition,
              wxSize(900, 700),
              wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT),
      definition_(std::move(definition)),
      callback_(std::move(callback)) {
  CreateStatusBar();
  SetStatusText("Ready");

  if (!definition_.menus.empty()) {
    auto* bar = new wxMenuBar;
    for (const auto& menu_definition : definition_.menus) {
      auto* menu = new wxMenu;
      for (const auto& item : menu_definition.items) {
        if (item.separator) {
          menu->AppendSeparator();
          continue;
        }
        const int id = wxWindow::NewControlId();
        wxString label = Text(item.label);
        if (!item.accelerator.empty())
          label += "\t" + Text(item.accelerator);
        menu->Append(id, label, wxEmptyString,
                     item.checkable ? wxITEM_CHECK : wxITEM_NORMAL);
        Bind(wxEVT_MENU,
             [this, action = item.id](wxCommandEvent& event) {
               SendEvent(action,
                         event.IsChecked() ? "true" : "{\"pressed\":true}");
             },
             id);
      }
      bar->Append(menu, Text(menu_definition.label));
    }
    SetMenuBar(bar);
  }

  auto* outer = new wxPanel(this);
  auto* outer_sizer = new wxBoxSizer(wxVERTICAL);
  if (!definition_.tabs.empty()) {
    auto* notebook = new wxNotebook(outer, wxID_ANY);
    std::map<std::string, wxScrolledWindow*> pages;
    std::map<std::string, wxBoxSizer*> page_sizers;
    for (const auto& tab : definition_.tabs) {
      auto* page = new wxScrolledWindow(notebook);
      page->SetScrollRate(8, 8);
      auto* sizer = new wxBoxSizer(wxVERTICAL);
      page->SetSizer(sizer);
      notebook->AddPage(page, Text(tab));
      pages.emplace(tab, page);
      page_sizers.emplace(tab, sizer);
    }
    for (const auto& control : definition_.controls) {
      auto* page = pages.at(control.tab);
      page_sizers.at(control.tab)
          ->Add(BuildControl(page, control), control.type == "table" ? 1 : 0,
                wxEXPAND | wxALL, 5);
    }
    outer_sizer->Add(notebook, 1, wxEXPAND | wxALL, 8);
  } else {
    auto* page = new wxScrolledWindow(outer);
    page->SetScrollRate(8, 8);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    page->SetSizer(sizer);
    for (const auto& control : definition_.controls)
      sizer->Add(BuildControl(page, control),
                 control.type == "table" ? 1 : 0,
                 wxEXPAND | wxALL, 5);
    outer_sizer->Add(page, 1, wxEXPAND | wxALL, 8);
  }
  outer->SetSizer(outer_sizer);
  auto* frame_sizer = new wxBoxSizer(wxVERTICAL);
  frame_sizer->Add(outer, 1, wxEXPAND);
  SetSizer(frame_sizer);
  Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
    if (event.CanVeto()) {
      Hide();
      event.Veto();
    } else {
      event.Skip();
    }
  });
  CentreOnParent();
}

wxWindow* SurfaceDialog::BuildControl(wxWindow* parent,
                                      const UiControl& control) {
  auto* row = new wxPanel(parent);
  auto* sizer = new wxBoxSizer(
      control.type == "table" || control.type == "diagnostics"
          ? wxVERTICAL
          : wxHORIZONTAL);
  row->SetSizer(sizer);

  wxWindow* widget = nullptr;
  if (control.type == "toggle") {
    widget = new wxCheckBox(row, wxID_ANY, Text(control.label));
    sizer->Add(widget, 1, wxALIGN_CENTER_VERTICAL);
    widget->Bind(wxEVT_CHECKBOX,
                 [this, id = control.id](wxCommandEvent& event) {
                   SendEvent(id, event.IsChecked() ? "true" : "false");
                 });
  } else {
    if (!control.icon_only)
      sizer->Add(new wxStaticText(row, wxID_ANY, Text(control.label)), 0,
                 wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    if (IsButton(control.type)) {
      widget = new wxButton(row, wxID_ANY, Text(control.label));
      sizer->Add(widget, 0, wxALIGN_CENTER_VERTICAL);
      widget->Bind(wxEVT_BUTTON,
                   [this, id = control.id, type = control.type](
                       wxCommandEvent&) {
                     if (type == "file-open" ||
                         type == "file-open-multiple") {
                       long style = wxFD_OPEN | wxFD_FILE_MUST_EXIST;
                       if (type == "file-open-multiple")
                         style |= wxFD_MULTIPLE;
                       wxFileDialog picker(this, "Choose file", wxEmptyString,
                                           wxEmptyString, "All files|*",
                                           style);
                       if (picker.ShowModal() == wxID_OK)
                         SendEvent(id, JsonString(picker.GetPath()));
                     } else {
                       SendEvent(id, "{\"pressed\":true}");
                     }
                   });
    } else if (control.type == "choice" ||
               control.type == "position-source" ||
               control.type == "navigation-object" ||
               control.type == "navigation-route") {
      widget = new wxChoice(row, wxID_ANY);
      sizer->Add(widget, 1, wxEXPAND);
      widget->Bind(wxEVT_CHOICE,
                   [this, id = control.id](wxCommandEvent& event) {
                     SendEvent(id, JsonString(event.GetString()));
                   });
    } else if (control.type == "progress") {
      widget = new wxGauge(row, wxID_ANY, 100);
      sizer->Add(widget, 1, wxEXPAND);
    } else if (control.type == "slider") {
      widget = new wxSlider(row, wxID_ANY, 0, 0, 100);
      sizer->Add(widget, 1, wxEXPAND);
      widget->Bind(wxEVT_SLIDER,
                   [this, id = control.id](wxCommandEvent& event) {
                     SendEvent(id, std::to_string(event.GetInt()));
                   });
    } else if (control.type == "table") {
      auto* table = new wxListCtrl(
          row, wxID_ANY, wxDefaultPosition, wxSize(-1, 180),
          wxLC_REPORT | wxLC_SINGLE_SEL);
      for (std::size_t index = 0; index < control.columns.size(); ++index)
        table->InsertColumn(static_cast<long>(index),
                            Text(control.columns[index]));
      widget = table;
      sizer->Add(widget, 1, wxEXPAND);
    } else if (control.type == "diagnostics") {
      widget = new wxTextCtrl(row, wxID_ANY, wxEmptyString,
                              wxDefaultPosition, wxSize(-1, 100),
                              wxTE_MULTILINE | wxTE_READONLY);
      sizer->Add(widget, 1, wxEXPAND);
    } else if (control.type == "status") {
      widget = new wxStaticText(row, wxID_ANY, "—");
      sizer->Add(widget, 1, wxALIGN_CENTER_VERTICAL);
    } else {
      widget = new wxTextCtrl(row, wxID_ANY, wxEmptyString,
                              wxDefaultPosition, wxDefaultSize,
                              wxTE_PROCESS_ENTER);
      sizer->Add(widget, 1, wxEXPAND);
      widget->Bind(wxEVT_TEXT_ENTER,
                   [this, id = control.id](wxCommandEvent& event) {
                     SendEvent(id, JsonString(event.GetString()));
                   });
    }
  }
  controls_[control.id] = widget;
  return row;
}

void SurfaceDialog::SendEvent(const std::string& control_id,
                              const std::string& value_json) {
  SetStatusText("Working…");
  if (callback_) callback_(control_id, value_json);
}

void SurfaceDialog::ApplyResponse(const std::string& control_id,
                                  const std::string& state_json,
                                  const std::string& diagnostic) {
  if (!diagnostic.empty()) {
    SetStatusText("Error: " + Text(diagnostic));
    return;
  }
  SetStatusText("Updated " + Text(control_id));
  const auto item = controls_.find(control_id);
  if (item == controls_.end()) return;
  if (auto* status = dynamic_cast<wxStaticText*>(item->second))
    status->SetLabel(Text(state_json));
}

}  // namespace ppm
