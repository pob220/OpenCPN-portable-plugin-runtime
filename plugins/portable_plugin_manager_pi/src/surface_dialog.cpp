#include "surface_dialog.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/grid.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/settings.h>
#include <wx/sstream.h>
#include <wx/textctrl.h>

namespace ppm {
namespace {

wxString Text(const std::string& value) { return wxString::FromUTF8(value); }

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
  return type == "button" || type == "cancel" || type == "file-open" ||
         type == "file-open-multiple" || type == "file-save" ||
         type == "navigation-create";
}

long SurfaceWindowStyle(const DeclarativeSurface& definition) {
  if (definition.role == "tool-window" || definition.role == "dockable-panel" ||
      definition.role == "inspector") {
    return wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER | wxFRAME_TOOL_WINDOW |
           wxFRAME_FLOAT_ON_PARENT;
  }
  if (definition.role == "modal-task")
    return wxCAPTION | wxCLOSE_BOX | wxFRAME_FLOAT_ON_PARENT;
  return wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT;
}

class PolarPlotPanel final : public wxPanel {
public:
  explicit PolarPlotPanel(wxWindow* parent, const wxString& accessible_name)
      : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 420)) {
    SetName(accessible_name);
    SetToolTip(
        "Boat speed through water by true wind angle and true wind speed");
    SetMinSize(wxSize(480, 360));
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &PolarPlotPanel::OnPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
      Refresh(false);
      event.Skip();
    });
  }

  void Apply(const wxJSONValue& value) {
    winds_.clear();
    angles_.clear();
    speeds_.clear();
    if (!value.IsObject() || !value.HasMember("columns") ||
        !value.HasMember("rows") || !value.ItemAt("columns").IsArray() ||
        !value.ItemAt("rows").IsArray()) {
      Refresh(false);
      return;
    }
    const wxJSONValue columns = value.ItemAt("columns");
    for (int index = 1; index < columns.Size(); ++index) {
      wxString text = columns.ItemAt(index).AsString();
      text.Replace("kn", "");
      double wind = 0.0;
      if (text.Trim(true).Trim(false).ToDouble(&wind) && wind > 0.0)
        winds_.push_back(wind);
    }
    const wxJSONValue rows = value.ItemAt("rows");
    for (int row = 0; row < rows.Size(); ++row) {
      const wxJSONValue cells = rows.ItemAt(row);
      if (!cells.IsArray() || cells.Size() == 0) continue;
      double angle = 0.0;
      if (!cells.ItemAt(0).AsString().ToDouble(&angle) || angle < 0.0 ||
          angle > 180.0)
        continue;
      std::vector<std::optional<double>> values(winds_.size());
      for (std::size_t column = 0; column < winds_.size(); ++column) {
        if (static_cast<int>(column + 1) >= cells.Size()) break;
        double speed = 0.0;
        const wxString text =
            cells.ItemAt(static_cast<int>(column + 1)).AsString();
        if (!text.empty() && text.ToDouble(&speed) && speed >= 0.0)
          values[column] = speed;
      }
      angles_.push_back(angle);
      speeds_.push_back(std::move(values));
    }
    Refresh(false);
  }

private:
  static wxColour CurveColour(std::size_t index) {
    static const wxColour colours[] = {
        wxColour(24, 101, 171), wxColour(0, 136, 122),  wxColour(236, 112, 20),
        wxColour(178, 54, 147), wxColour(93, 63, 152),  wxColour(44, 145, 48),
        wxColour(200, 65, 58),  wxColour(34, 139, 160), wxColour(137, 104, 22),
        wxColour(118, 79, 143), wxColour(25, 125, 95),  wxColour(209, 92, 135)};
    return colours[index % (sizeof(colours) / sizeof(colours[0]))];
  }

  void OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
    dc.Clear();
    const wxSize size = GetClientSize();
    if (size.x < 120 || size.y < 120) return;

    dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
    dc.SetFont(wxFontInfo(11).Bold());
    dc.DrawText("Polar diagram — STW by TWA and TWS", 12, 8);
    dc.SetFont(*wxSMALL_FONT);
    if (winds_.empty() || angles_.empty() || speeds_.empty()) {
      dc.DrawText("Open or create a polar to display its performance curves.",
                  12, 40);
      return;
    }

    double maximum_speed = 0.0;
    for (const auto& row : speeds_)
      for (const auto& value : row)
        if (value) maximum_speed = std::max(maximum_speed, *value);
    if (maximum_speed <= 0.0) {
      dc.DrawText("The current polar contains no non-zero speed samples.", 12,
                  40);
      return;
    }
    const double scale_max = std::max(1.0, std::ceil(maximum_speed));
    const int legend_width = size.x >= 720 ? 145 : 90;
    const int radius = std::max(
        45, std::min((size.x - legend_width - 28) / 2, (size.y - 62) / 2));
    const wxPoint centre(16 + radius, 46 + radius);
    const wxColour grid = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
    dc.SetPen(wxPen(grid, 1, wxPENSTYLE_DOT));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    for (int ring = 1; ring <= 4; ++ring) {
      const int ring_radius = radius * ring / 4;
      dc.DrawCircle(centre, ring_radius);
      dc.DrawText(wxString::Format("%.1f", scale_max * ring / 4.0),
                  centre.x + 3, centre.y - ring_radius - 2);
    }
    constexpr double kPi = 3.14159265358979323846;
    for (int angle = 0; angle <= 180; angle += 30) {
      const double radians = angle * kPi / 180.0;
      const int dx = static_cast<int>(std::lround(std::sin(radians) * radius));
      const int dy = static_cast<int>(std::lround(std::cos(radians) * radius));
      dc.DrawLine(centre.x - dx, centre.y - dy, centre.x + dx, centre.y - dy);
      if (angle % 60 == 0)
        dc.DrawText(wxString::Format("%d°", angle), centre.x + dx + 2,
                    centre.y - dy - 7);
    }
    dc.SetPen(wxPen(grid, 1));
    dc.DrawLine(centre.x, centre.y - radius, centre.x, centre.y + radius);
    dc.DrawText("STW kn", centre.x - 18, centre.y + radius + 6);

    for (std::size_t wind = 0; wind < winds_.size(); ++wind) {
      std::vector<wxPoint> starboard;
      std::vector<wxPoint> port;
      for (std::size_t row = 0; row < angles_.size() && row < speeds_.size();
           ++row) {
        if (wind >= speeds_[row].size() || !speeds_[row][wind]) continue;
        const double radians = angles_[row] * kPi / 180.0;
        const double distance =
            std::clamp(*speeds_[row][wind] / scale_max, 0.0, 1.0) * radius;
        const int dx =
            static_cast<int>(std::lround(std::sin(radians) * distance));
        const int dy =
            static_cast<int>(std::lround(std::cos(radians) * distance));
        starboard.emplace_back(centre.x + dx, centre.y - dy);
        port.emplace_back(centre.x - dx, centre.y - dy);
      }
      const wxColour colour = CurveColour(wind);
      dc.SetPen(wxPen(colour, 2));
      if (starboard.size() >= 2)
        dc.DrawLines(static_cast<int>(starboard.size()), starboard.data());
      if (port.size() >= 2)
        dc.DrawLines(static_cast<int>(port.size()), port.data());
      dc.SetBrush(wxBrush(colour));
      for (const auto& point : starboard) dc.DrawCircle(point, 2);
      for (const auto& point : port) dc.DrawCircle(point, 2);

      const int legend_x = centre.x + radius + 18;
      const int legend_y = 40 + static_cast<int>(wind) * 17;
      if (legend_y + 14 < size.y) {
        dc.SetPen(wxPen(colour, 3));
        dc.DrawLine(legend_x, legend_y + 6, legend_x + 18, legend_y + 6);
        dc.SetTextForeground(
            wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        dc.DrawText(wxString::Format("%.1f kn", winds_[wind]), legend_x + 24,
                    legend_y);
      }
    }
  }

  std::vector<double> winds_;
  std::vector<double> angles_;
  std::vector<std::vector<std::optional<double>>> speeds_;
};

}  // namespace

SurfaceDialog::SurfaceDialog(wxWindow* parent, DeclarativeSurface definition,
                             EventCallback callback)
    : wxFrame(parent, wxID_ANY, Text(definition.title), wxDefaultPosition,
              wxSize(900, 700), SurfaceWindowStyle(definition)),
      definition_(std::move(definition)),
      callback_(std::move(callback)) {
  SetName(Text("OPP surface " + definition_.id));
  Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
    if (event.CanVeto()) {
      Hide();
      event.Veto();
    } else {
      event.Skip();
    }
  });
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
        if (!item.accelerator.empty()) label += "\t" + Text(item.accelerator);
        menu->Append(id, label, wxEmptyString,
                     item.checkable ? wxITEM_CHECK : wxITEM_NORMAL);
        Bind(
            wxEVT_MENU,
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
          ->Add(BuildControl(page, control),
                (control.type == "table" || control.type == "grid" ||
                 control.type == "polar-plot")
                    ? 1
                    : 0,
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
                 (control.type == "table" || control.type == "grid" ||
                  control.type == "polar-plot")
                     ? 1
                     : 0,
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
      control.type == "table" || control.type == "grid" ||
              control.type == "diagnostics" || control.type == "polar-plot"
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
      widget->Bind(wxEVT_BUTTON, [this, id = control.id, type = control.type,
                                  filter =
                                      control.file_filter](wxCommandEvent&) {
        if (type == "file-open" || type == "file-open-multiple" ||
            type == "file-save") {
          long style = type == "file-save" ? wxFD_SAVE | wxFD_OVERWRITE_PROMPT
                                           : wxFD_OPEN | wxFD_FILE_MUST_EXIST;
          if (type == "file-open-multiple") style |= wxFD_MULTIPLE;
          wxFileDialog picker(
              this, type == "file-save" ? "Save file" : "Choose file",
              wxEmptyString, wxEmptyString,
              filter.empty() ? "All files|*" : Text(filter), style);
          if (picker.ShowModal() == wxID_OK) {
            wxArrayString paths;
            if (type == "file-open-multiple")
              picker.GetPaths(paths);
            else
              paths.Add(picker.GetPath());
            std::vector<UserFileSelection> selections;
            selections.reserve(paths.size());
            for (const auto& path : paths) {
              const wxScopedCharBuffer bytes = path.utf8_str();
              selections.push_back(
                  {bytes ? std::string(bytes.data(), bytes.length())
                         : std::string(),
                   type == "file-save"});
            }
            SendEvent(id, "null", std::move(selections));
          }
        } else {
          SendEvent(id, "{\"pressed\":true}");
        }
      });
    } else if (control.type == "choice" || control.type == "position-source" ||
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
      auto* table =
          new wxListCtrl(row, wxID_ANY, wxDefaultPosition, wxSize(-1, 180),
                         wxLC_REPORT | wxLC_SINGLE_SEL);
      for (std::size_t index = 0; index < control.columns.size(); ++index)
        table->InsertColumn(static_cast<long>(index),
                            Text(control.columns[index]));
      widget = table;
      sizer->Add(widget, 1, wxEXPAND);
      widget->Bind(wxEVT_LIST_ITEM_SELECTED, [this, id = control.id](
                                                 wxListEvent& event) {
        if (!applying_response_)
          SendEvent(id, "{\"row\":" + std::to_string(event.GetIndex()) + "}");
        event.Skip();
      });
    } else if (control.type == "grid") {
      auto* grid =
          new wxGrid(row, wxID_ANY, wxDefaultPosition, wxSize(-1, 300));
      grid->CreateGrid(0, static_cast<int>(control.columns.size()));
      for (std::size_t index = 0; index < control.columns.size(); ++index)
        grid->SetColLabelValue(static_cast<int>(index),
                               Text(control.columns[index]));
      grid->EnableEditing(true);
      grid->Bind(
          wxEVT_GRID_CELL_CHANGED, [this, id = control.id](wxGridEvent& event) {
            if (!applying_response_) {
              auto* source = dynamic_cast<wxGrid*>(event.GetEventObject());
              if (source) {
                const std::string value =
                    "{\"row\":" + std::to_string(event.GetRow()) +
                    ",\"column\":" + std::to_string(event.GetCol()) +
                    ",\"value\":" +
                    JsonString(
                        source->GetCellValue(event.GetRow(), event.GetCol())) +
                    "}";
                SendEvent(id, value);
              }
            }
            event.Skip();
          });
      grid->Bind(wxEVT_GRID_SELECT_CELL, [this,
                                          id = control.id](wxGridEvent& event) {
        if (!applying_response_) {
          SendEvent(id, "{\"row\":" + std::to_string(event.GetRow()) +
                            ",\"column\":" + std::to_string(event.GetCol()) +
                            "}");
        }
        event.Skip();
      });
      widget = grid;
      sizer->Add(widget, 1, wxEXPAND);
    } else if (control.type == "polar-plot") {
      widget = new PolarPlotPanel(row, Text(control.label));
      sizer->Add(widget, 1, wxEXPAND);
    } else if (control.type == "diagnostics") {
      widget = new wxTextCtrl(row, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxSize(-1, 100), wxTE_MULTILINE | wxTE_READONLY);
      sizer->Add(widget, 1, wxEXPAND);
    } else if (control.type == "status") {
      widget = new wxStaticText(row, wxID_ANY, "—");
      sizer->Add(widget, 1, wxALIGN_CENTER_VERTICAL);
    } else {
      widget = new wxTextCtrl(row, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxDefaultSize, wxTE_PROCESS_ENTER);
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
                              const std::string& value_json,
                              std::vector<UserFileSelection> selections) {
  SetStatusText("Working…");
  if (callback_) callback_(control_id, value_json, selections);
}

void SurfaceDialog::ApplyResponse(const std::string& control_id,
                                  const std::string& state_json,
                                  const std::string& diagnostic) {
  if (!diagnostic.empty()) {
    SetStatusText("Error: " + Text(diagnostic));
    return;
  }
  wxJSONValue state;
  wxJSONReader reader;
  wxStringInputStream stream(Text(state_json));
  if (reader.Parse(stream, &state) != 0 || !state.IsObject()) {
    SetStatusText("Package returned an invalid UI state");
    return;
  }
  if (state.HasMember("status") && state["status"].IsString())
    SetStatusText(state["status"].AsString());
  else
    SetStatusText("Updated " + Text(control_id));
  if (!state.HasMember("controls") || !state["controls"].IsObject()) return;

  applying_response_ = true;
  const wxJSONValue values = state["controls"];
  for (const auto& name : values.GetMemberNames()) {
    const auto item = controls_.find(name.ToStdString());
    if (item == controls_.end()) continue;
    const wxJSONValue value = values.ItemAt(name);
    if (auto* plot = dynamic_cast<PolarPlotPanel*>(item->second)) {
      plot->Apply(value);
    } else if (auto* grid = dynamic_cast<wxGrid*>(item->second)) {
      const wxJSONValue rows = value.ItemAt("rows");
      if (!value.IsObject() || !value.HasMember("rows") || !rows.IsArray())
        continue;
      if (value.HasMember("columns")) {
        const wxJSONValue columns = value.ItemAt("columns");
        if (columns.IsArray() && columns.Size() > 0 && columns.Size() <= 64) {
          const int wanted_columns = columns.Size();
          if (grid->GetNumberCols() < wanted_columns)
            grid->AppendCols(wanted_columns - grid->GetNumberCols());
          else if (grid->GetNumberCols() > wanted_columns)
            grid->DeleteCols(0, grid->GetNumberCols() - wanted_columns);
          for (int column = 0; column < wanted_columns; ++column)
            grid->SetColLabelValue(column, columns.ItemAt(column).AsString());
        }
      }
      const int wanted_rows = rows.Size();
      if (grid->GetNumberRows() < wanted_rows)
        grid->AppendRows(wanted_rows - grid->GetNumberRows());
      else if (grid->GetNumberRows() > wanted_rows)
        grid->DeleteRows(0, grid->GetNumberRows() - wanted_rows);
      for (int row = 0; row < wanted_rows; ++row) {
        const wxJSONValue cells = rows.ItemAt(row);
        if (!cells.IsArray()) continue;
        for (int column = 0;
             column < cells.Size() && column < grid->GetNumberCols();
             ++column) {
          grid->SetCellValue(row, column, cells.ItemAt(column).AsString());
        }
      }
      grid->AutoSizeColumns(false);
    } else if (auto* table = dynamic_cast<wxListCtrl*>(item->second)) {
      table->DeleteAllItems();
      const wxJSONValue rows = value.ItemAt("rows");
      if (!value.IsObject() || !value.HasMember("rows") || !rows.IsArray())
        continue;
      for (int row = 0; row < rows.Size(); ++row) {
        const wxJSONValue cells = rows.ItemAt(row);
        if (!cells.IsArray() || cells.Size() == 0) continue;
        const long inserted = table->InsertItem(table->GetItemCount(),
                                                cells.ItemAt(0).AsString());
        for (int column = 1; column < cells.Size(); ++column)
          table->SetItem(inserted, column, cells.ItemAt(column).AsString());
      }
    } else if (auto* status = dynamic_cast<wxStaticText*>(item->second)) {
      if (value.IsString()) status->SetLabel(value.AsString());
    } else if (auto* text = dynamic_cast<wxTextCtrl*>(item->second)) {
      if (value.IsString()) text->ChangeValue(value.AsString());
    } else if (auto* gauge = dynamic_cast<wxGauge*>(item->second)) {
      if (value.IsInt()) gauge->SetValue(std::clamp(value.AsInt(), 0, 100));
    } else if (auto* toggle = dynamic_cast<wxCheckBox*>(item->second)) {
      if (value.IsBool()) toggle->SetValue(value.AsBool());
    }
  }
  applying_response_ = false;
  Layout();
}

}  // namespace ppm
