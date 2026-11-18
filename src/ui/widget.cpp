#include "litholattice/ui/widget.h"
#include <algorithm>
#include "litholattice/ui/ui_root.h"

namespace litholattice::ui {
namespace {

/// One axis of anchored placement: [start, end) inside the parent span [p0, p0 + plen).
void placeAxis(float p0, float plen, float aMin, float aMax, float insetLo, float insetHi, float offset, float pivot,
               float size, float& start, float& len) {
  if (aMin == aMax) {
    len = size;
    start = p0 + aMin * plen + offset - pivot * size;
  } else {
    start = p0 + aMin * plen + insetLo;
    len = std::max(0.f, p0 + aMax * plen - insetHi - start);
  }
}

}  // namespace

Rect anchoredRect(const Layout& l, const Rect& parent, Vec2 size) {
  Rect r;
  placeAxis(parent.x, parent.w, l.anchorMin.x, l.anchorMax.x, l.insets.left, l.insets.right, l.offset.x, l.pivot.x, size.x, r.x, r.w);
  placeAxis(parent.y, parent.h, l.anchorMin.y, l.anchorMax.y, l.insets.top, l.insets.bottom, l.offset.y, l.pivot.y, size.y, r.y, r.h);
  return r;
}

cWidget::cWidget(std::string styleClass) : styleClass_(std::move(styleClass)) {}

cWidget::~cWidget() {
  children_.clear();  // children first, while ui_ is still valid for them
  if (ui_) ui_->forget(this);
}

cWidget& cWidget::addChild(std::unique_ptr<cWidget> child) {
  child->parent_ = this;
  child->attach(ui_);
  children_.push_back(std::move(child));
  return *children_.back();
}

std::unique_ptr<cWidget> cWidget::removeChild(const cWidget& child) {
  const auto it = std::find_if(children_.begin(), children_.end(), [&](const auto& c) { return c.get() == &child; });
  if (it == children_.end()) return nullptr;
  std::unique_ptr<cWidget> w = std::move(*it);
  children_.erase(it);
  if (ui_) ui_->forget(w.get());
  w->parent_ = nullptr;
  w->attach(nullptr);
  return w;
}

void cWidget::clearChildren() { children_.clear(); }

void cWidget::attach(cUiRoot* ui) {
  if (ui_ && ui_ != ui) ui_->forget(this);
  ui_ = ui;
  for (auto& c : children_) c->attach(ui);
}

cWidget* cWidget::find(std::string_view id) {
  if (id_ == id) return this;
  for (auto& c : children_)
    if (cWidget* w = c->find(id)) return w;
  return nullptr;
}

cWidget& cWidget::setVisible(bool v) {
  visible_ = v;
  if (!v && ui_) ui_->forget(this);  // drops focus / hover / capture inside a hidden subtree
  return *this;
}

bool cWidget::interactive() const {
  for (const cWidget* w = this; w; w = w->parent_)
    if (!w->visible_ || !w->enabled_) return false;
  return true;
}

const Theme& cWidget::theme() const {
  static const Theme kFallback = Theme::defaults();
  return ui_ ? ui_->theme() : kFallback;
}

const Style& cWidget::style() const { return theme().style(styleClass_); }

bool cWidget::hovered() const { return ui_ && ui_->hover() == this; }
bool cWidget::pressed() const { return ui_ && ui_->capture() == this; }
bool cWidget::focused() const { return ui_ && ui_->focus() == this; }
void cWidget::focus() {
  if (ui_ && focusable_) ui_->setFocus(this);
}

cWidget& cWidget::on(std::string event, Handler h) {
  handlers_.emplace_back(std::move(event), std::move(h));
  return *this;
}

cWidget& cWidget::onScript(std::string event, std::string functionDecl) {
  scriptHandlers_.emplace_back(std::move(event), std::move(functionDecl));
  return *this;
}

void cWidget::emit(std::string_view event) {
  for (auto& [name, h] : handlers_)
    if (name == event) h(*this);
  for (auto& [name, decl] : scriptHandlers_)
    if (name == event && ui_ && ui_->scriptHost()) ui_->scriptHost()->callHandler(decl, id_);
}

bool cWidget::hasHandler(std::string_view event) const {
  const auto named = [&](const auto& p) { return p.first == event; };
  return std::any_of(handlers_.begin(), handlers_.end(), named) ||
         std::any_of(scriptHandlers_.begin(), scriptHandlers_.end(), named);
}

Vec2 cWidget::childSize(const cWidget& child, const Theme& theme) {
  const Layout& l = child.layout();
  Vec2 s = (l.size.x > 0 && l.size.y > 0) ? l.size : child.measure(theme);
  if (l.size.x > 0) s.x = l.size.x;
  if (l.size.y > 0) s.y = l.size.y;
  return {std::max(s.x, l.minSize.x), std::max(s.y, l.minSize.y)};
}

Vec2 cWidget::measure(const Theme& theme) const {
  // Fixed-size anchored children define the extent; stretched ones adapt to us.
  Vec2 extent;
  for (const auto& c : children_) {
    if (!c->visible()) continue;
    const Layout& l = c->layout();
    const Vec2 s = childSize(*c, theme);
    const float w = l.anchorMin.x == l.anchorMax.x ? l.offset.x + s.x : s.x + l.insets.horizontal();
    const float h = l.anchorMin.y == l.anchorMax.y ? l.offset.y + s.y : s.y + l.insets.vertical();
    extent = {std::max(extent.x, w), std::max(extent.y, h)};
  }
  const Edges& pad = theme.style(styleClass_).padding;
  return {extent.x + pad.horizontal(), extent.y + pad.vertical()};
}

void cWidget::place(const Rect& r, const Theme& theme) {
  rect_ = r;
  arrange(theme);
}

void cWidget::arrange(const Theme& theme) {
  const Rect area = contentRect();
  for (auto& c : children_) {
    if (!c->visible()) continue;
    c->place(anchoredRect(c->layout(), area, childSize(*c, theme)), theme);
  }
}

Color cWidget::stateBackground() const {
  const Style& s = style();
  if (!interactive()) return s.disabled.a > 0 && s.background.a > 0 ? s.disabled : s.background;
  if (pressed()) return s.pressed;
  if (hovered()) return s.hover;
  return s.background;
}

void cWidget::paint(Painter& p) const {
  const Style& s = style();
  p.fill(rect_, s.background);
  if (s.borderWidth > 0) p.frame(rect_, s.border, s.borderWidth);
}

void cWidget::paintChildren(Painter& p) const {
  for (const auto& c : children_) c->paintTree(p);
}

void cWidget::paintTree(Painter& p) const {
  if (!visible_) return;
  paint(p);
  paintChildren(p);
  if (focused()) p.frame(rect_, style().accent, 1.f);
}

cWidget* cWidget::hitTest(Vec2 p) {
  if (!visible_ || !rect_.contains(p)) return nullptr;
  for (auto it = children_.rbegin(); it != children_.rend(); ++it)
    if (cWidget* w = (*it)->hitTest(p)) return w;
  return enabled_ ? this : nullptr;
}

}  // namespace litholattice::ui
