//
// Copyleft RIME Developers
// License: GPLv3
//
// 2011-12-07 GONG Chen <chen.sst@gmail.com>
//
#include <string>
#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/processor.h>
#include <rime/schema.h>
#include <rime/switcher.h>
#include <rime/ticket.h>
#include <rime/translation.h>
#include <rime/translator.h>

namespace rime {

Switcher::Switcher() : Engine(new Schema) {
  context_->set_option("dumb", true);  // not going to commit anything

  // receive context notifications
  context_->select_notifier().connect(
      [this](Context* ctx) { OnSelect(ctx); });

  user_config_.reset(Config::Require("config")->Create("user"));
  InitializeComponents();
  LoadSettings();
}

Switcher::~Switcher() {
}

void Switcher::Attach(Engine* engine) {
  attached_engine_ = engine;
  // restore saved options
  if (user_config_) {
    for (const std::string& option_name : save_options_) {
      bool value = false;
      if (user_config_->GetBool("var/option/" + option_name, &value)) {
        engine->context()->set_option(option_name, value);
      }
    }
  }
}

bool Switcher::ProcessKeyEvent(const KeyEvent& key_event) {
  for (const KeyEvent& hotkey : hotkeys_) {
    if (key_event == hotkey) {
      if (!active_ && attached_engine_) {
        Activate();
      }
      else if (active_) {
        HighlightNextSchema();
      }
      return true;
    }
  }
  if (active_) {
    for (auto& p : processors_) {
      if (kNoop != p->ProcessKeyEvent(key_event))
        return true;
    }
    if (key_event.release() || key_event.ctrl() || key_event.alt())
      return true;
    int ch = key_event.keycode();
    if (ch == XK_space || ch == XK_Return) {
      context_->ConfirmCurrentSelection();
    }
    else if (ch == XK_Escape) {
      Deactivate();
    }
    return true;
  }
  return false;
}

void Switcher::HighlightNextSchema() {
  Composition* comp = context_->composition();
  if (!comp || comp->empty() || !comp->back().menu)
    return;
  Segment& seg(comp->back());
  int index = seg.selected_index;
  shared_ptr<Candidate> option;
  do {
    ++index;  // next
    int candidate_count = seg.menu->Prepare(index + 1);
    if (candidate_count <= index) {
      index = 0;  // passed the end; rewind
      break;
    }
    else {
      option = seg.GetCandidateAt(index);
    }
  }
  while (!option || option->type() != "schema");
  seg.selected_index = index;
  seg.tags.insert("paging");
  return;
}

Schema* Switcher::CreateSchema() {
  Config* config = schema_->config();
  if (!config) return NULL;
  auto schema_list = config->GetList("schema_list");
  if (!schema_list) return NULL;
  std::string previous;
  if (user_config_) {
    user_config_->GetString("var/previously_selected_schema", &previous);
  }
  std::string recent;
  for (size_t i = 0; i < schema_list->size(); ++i) {
    auto item = As<ConfigMap>(schema_list->GetAt(i));
    if (!item) continue;
    auto schema_property = item->GetValue("schema");
    if (!schema_property) continue;
    const std::string& schema_id(schema_property->str());
    if (previous.empty() || previous == schema_id) {
      recent = schema_id;
      break;
    }
    if (recent.empty())
      recent = schema_id;
  }
  if (recent.empty())
    return NULL;
  else
    return new Schema(recent);
}

void Switcher::ApplySchema(Schema* schema) {
  if (!schema) return;
  if (active_) {
    Deactivate();
  }
  attached_engine_->ApplySchema(schema);
}

void Switcher::SelectNextSchema() {
  if (translators_.empty())
    return;
  auto xlator = translators_[0];  // schema_list_translator
  if (!xlator)
    return;
  Menu menu;
  menu.AddTranslation(xlator->Query("", Segment(), NULL));
  if (menu.Prepare(2) < 2)
    return;
  auto command = As<SwitcherCommand>(menu.GetCandidateAt(1));
  if (!command)
    return;
  command->Apply(this);
}

bool Switcher::IsAutoSave(const std::string& option) const {
  return save_options_.find(option) != save_options_.end();
}

void Switcher::OnSelect(Context* ctx) {
  LOG(INFO) << "a switcher option is selected.";
  Segment& seg(ctx->composition()->back());
  auto command = As<SwitcherCommand>(seg.GetSelectedCandidate());
  if (!command)
    return;
  if (attached_engine_) {
    command->Apply(this);
  }
  Deactivate();
}

void Switcher::Activate() {
  LOG(INFO) << "switcher is activated.";
  Composition* comp = context_->composition();
  if (comp->empty()) {
    context_->set_input(" ");  // make context_->IsComposing() == true
    Segment seg(0, 0);         // empty range
    seg.prompt = caption_;
    comp->AddSegment(seg);
  }
  auto menu = make_shared<Menu>();
  comp->back().menu = menu;
  for (auto& translator : translators_) {
    if (auto t = translator->Query("", comp->back(), NULL)) {
      menu->AddTranslation(t);
    }
  }

  // activated!
  active_ = true;
}

void Switcher::Deactivate() {
  context_->Clear();
  active_ = false;
}

void Switcher::LoadSettings() {
  Config* config = schema_->config();
  if (!config)
    return;
  if (!config->GetString("switcher/caption", &caption_) || caption_.empty()) {
    caption_ = ":-)";
  }
  auto hotkeys = config->GetList("switcher/hotkeys");
  if (!hotkeys)
    return;
  hotkeys_.clear();
  for (size_t i = 0; i < hotkeys->size(); ++i) {
    auto value = hotkeys->GetValueAt(i);
    if (!value)
      continue;
    hotkeys_.push_back(KeyEvent(value->str()));
  }
  auto options = config->GetList("switcher/save_options");
  if (!options)
    return;
  save_options_.clear();
  for (auto it = options->begin(); it != options->end(); ++it) {
    auto option_name = As<ConfigValue>(*it);
    if (!option_name)
      continue;
    save_options_.insert(option_name->str());
  }
}

void Switcher::InitializeComponents() {
  processors_.clear();
  translators_.clear();
  if (auto c = Processor::Require("key_binder")) {
    shared_ptr<Processor> p(c->Create(Ticket(this)));
    processors_.push_back(p);
  }
  else {
    LOG(WARNING) << "key_binder not available.";
  }
  if (auto c = Processor::Require("selector")) {
    shared_ptr<Processor> p(c->Create(Ticket(this)));
    processors_.push_back(p);
  }
  else {
    LOG(WARNING) << "selector not available.";
  }
  DLOG(INFO) << "num processors: " << processors_.size();
  if (auto c = Translator::Require("schema_list_translator")) {
    shared_ptr<Translator> t(c->Create(Ticket(this)));
    translators_.push_back(t);
  }
  else {
    LOG(WARNING) << "schema_list_translator not available.";
  }
  if (auto c = Translator::Require("switch_translator")) {
    shared_ptr<Translator> t(c->Create(Ticket(this)));
    translators_.push_back(t);
  }
  else {
    LOG(WARNING) << "switch_translator not available.";
  }
  DLOG(INFO) << "num translators: " << translators_.size();
}

}  // namespace rime
