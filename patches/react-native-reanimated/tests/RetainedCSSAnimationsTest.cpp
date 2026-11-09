// Exercises native exit retention with real React Native shadow nodes and Yoga.
#include <reanimated/Fabric/updates/UpdatesRegistryManager.h>
#include <react/renderer/components/root/RootComponentDescriptor.h>
#include <react/renderer/components/view/ViewComponentDescriptor.h>
#include <react/renderer/mounting/Differentiator.h>
#include <react/renderer/componentregistry/ComponentDescriptorProviderRegistry.h>
#include <reanimated/LayoutAnimations/LayoutAnimationsProxyCommon.h>
#include <jsc/JSCRuntime.h>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace facebook::react;
using namespace reanimated;

struct Registry : UpdatesRegistry {
  std::unordered_set<Tag> removed;
  void removeTag(Tag tag) override { removed.insert(tag); }
};

struct Tree {
  std::shared_ptr<const ContextContainer> context = std::make_shared<ContextContainer>();
  RootComponentDescriptor rootDescriptor{{.eventDispatcher = EventDispatcher::Shared{}, .contextContainer = context, .flavor = nullptr}};
  ViewComponentDescriptor viewDescriptor{{.eventDispatcher = EventDispatcher::Shared{}, .contextContainer = context, .flavor = nullptr}};
  std::shared_ptr<ViewShadowNode> indicator;
  RootShadowNode::Unshared root;

  explicit Tree(SurfaceId surface = 1) {
    const PropsParserContext parser{surface, *context};
    auto props = std::make_shared<RootProps>();
    props->layoutConstraints.minimumSize = {300, 20};
    props->layoutConstraints.maximumSize = {300, 20};
    auto indicatorProps = viewDescriptor.cloneProps(parser, nullptr,
        RawProps(folly::dynamic::object("width", "25%")("left", "0%")
          ("height", 4)("position", "absolute")("collapsable", false)));
    indicator = std::static_pointer_cast<ViewShadowNode>(viewDescriptor.createShadowNode(
        {.props = indicatorProps}, viewDescriptor.createFamily({surface + 100, surface, nullptr})));
    root = std::static_pointer_cast<RootShadowNode>(rootDescriptor.createShadowNode(
        {.props = props, .children = std::make_shared<const std::vector<std::shared_ptr<const ShadowNode>>>(
            std::vector<std::shared_ptr<const ShadowNode>>{indicator})},
        rootDescriptor.createFamily({surface, surface, nullptr})));
    root->layoutIfNeeded();
    root->sealRecursive();
  }

  RootShadowNode::Unshared remove() {
    auto removed = std::static_pointer_cast<RootShadowNode>(root->ShadowNode::clone(
        {.children = ShadowNode::emptySharedShadowNodeSharedList()}));
    removed->layoutIfNeeded();
    removed->sealRecursive();
    return removed;
  }
};

struct Proxy : LayoutAnimationsProxyCommon {
  mutable std::unordered_map<Tag, ShadowView> retained;
  explicit Proxy(const LayoutAnimationsProxyDependencies &dependencies)
      : LayoutAnimationsProxyCommon(1, dependencies) {}
  bool shouldOverridePullTransaction() const override { return true; }
  std::optional<MountingTransaction> pullTransaction(SurfaceId, MountingTransaction::Number,
      const TransactionTelemetry &, ShadowViewMutationList) const override { return std::nullopt; }
  std::optional<SurfaceId> endLayoutAnimation(int, bool) override { return std::nullopt; }
  std::optional<ShadowView> getRetainedView(Tag tag) const override {
    const auto it = retained.find(tag);
    return it == retained.end() ? std::nullopt : std::optional(it->second);
  }
  void updateRetainedView(const ShadowView &view) const override { retained.at(view.tag) = view; }
  ShadowViewMutationList flush() const {
    ShadowViewMutationList mutations;
    appendRetainedViewUpdates(mutations);
    return mutations;
  }
  void startExit(Tag tag) {
    auto view = retained.at(tag);
    view.props = mergeSynchronousProps(view, folly::dynamic::object("opacity", 0.4));
    retained.at(tag) = view;
    layoutAnimations_.insert_or_assign(tag, LayoutAnimation{
        view, view, view, 1, std::nullopt, LayoutAnimationType::EXITING, nullptr, {}});
  }
};

int main() {
  const auto staticProps = std::make_shared<css::StaticPropsRegistry>();
  auto registry = std::make_shared<Registry>();
  UpdatesRegistryManager manager(staticProps);
  manager.addRegistry(registry);
  Tree first;
  Tree second(11);
  const auto tag = first.indicator->getTag();
  const auto secondTag = second.indicator->getTag();
  auto removed = first.remove();

  // Given a CSS indicator mounted on each of two independent surfaces.
  {
    auto lock = manager.lock();
    manager.handleNodeRemovals(first.root, [](Tag) { return false; });
    manager.handleNodeRemovals(second.root, [](Tag) { return false; });
    manager.markNodeAsRemovable(first.indicator);
    manager.markNodeAsRemovable(second.indicator);
  }

  // When React removes the first indicator but a native exiting view retains it.
  {
    auto lock = manager.lock();
    manager.handleNodeRemovals(removed, [tag](Tag candidate) { return candidate == tag; });

    // Then neither its CSS animation nor the other surface's animation is removed.
    assert(!registry->removed.contains(tag));
    assert(!registry->removed.contains(secondTag));
    assert(manager.getRetainedRoots(1).size() == 1);
    assert(manager.getRetainedRoots(11).empty());
  }

  PropsMap props;
  props[first.indicator->getFamilyShared()].emplace_back(
      folly::dynamic::object("width", "75%")("left", "25%"));

  // Given the live tree no longer contains the indicator.
  auto live = cloneShadowTreeWithNewProps(*removed, props);
  live->layoutIfNeeded();
  assert(live->getChildren().empty());

  // When the next CSS frame is applied to its retained snapshot instead.
  auto updated = cloneShadowTreeWithNewProps(*first.root, props);
  updated->layoutIfNeeded();
  const auto view = std::static_pointer_cast<const ViewShadowNode>(updated->getChildren().front());

  // Then Yoga resolves percentages against the old parent and emits an update, without reinserting the view.
  assert(std::abs(view->getLayoutMetrics().frame.size.width - 225) < 0.01);
  assert(std::abs(view->getLayoutMetrics().frame.origin.x - 75) < 0.01);
  bool indicatorUpdated = false;
  for (const auto &mutation : calculateShadowViewMutations(*first.root, *updated)) {
    assert(mutation.type == ShadowViewMutation::Update);
    indicatorUpdated |= mutation.newChildShadowView.tag == tag;
  }
  assert(indicatorUpdated);

  ComponentDescriptorProviderRegistry providers;
  providers.add(concreteComponentDescriptorProvider<RootComponentDescriptor>());
  providers.add(concreteComponentDescriptorProvider<ViewComponentDescriptor>());
  auto descriptorRegistry = providers.createComponentDescriptorRegistry(
      {.eventDispatcher = EventDispatcher::Shared{}, .contextContainer = first.context, .flavor = nullptr});
  auto runtime = facebook::jsc::makeJSCRuntime();
  Proxy proxy({.layoutAnimationsManager = nullptr,
      .componentDescriptorRegistry = descriptorRegistry,
      .contextContainer = first.context,
      .uiRuntime = *runtime,
      .uiScheduler = nullptr,
      .uiManager = nullptr,
      .requestLayoutAnimationFlush = [](SurfaceId) {}});

  // Given CSS frames and layout exit frames can both arrive before the next native pull.
  proxy.retained.emplace(tag, ShadowView(*first.indicator));
  proxy.startExit(tag);
  proxy.enqueueRetainedViewUpdates({{ShadowView(*view), 1, folly::dynamic::object("width", "75%")}});
  proxy.enqueueRetainedViewUpdates({{ShadowView(*view), 1, folly::dynamic::object("left", "25%")}});

  // When the mounting proxy receives the next pull.
  auto mutations = proxy.flush();

  // Then it merges both CSS frames and preserves the exit animation's in-flight opacity.
  assert(mutations.size() == 1);
  assert(std::abs(mutations.front().newChildShadowView.layoutMetrics.frame.size.width - 225) < 0.01);
  assert(std::abs(static_cast<const ViewProps &>(*mutations.front().newChildShadowView.props).opacity - 0.4) < 0.01);
  const auto &yogaStyle = static_cast<const ViewProps &>(*mutations.front().newChildShadowView.props).yogaStyle;
  assert(yogaStyle.dimension(facebook::yoga::Dimension::Width).value().unwrap() == 75);
  assert(yogaStyle.position(facebook::yoga::Edge::Left).value().unwrap() == 25);

  // Given an exit ends after a CSS frame is queued but before the next native pull.
  proxy.enqueueRetainedViewUpdates({{ShadowView(*view), 1, folly::dynamic::object("width", "75%")}});

  // When the view is deleted, then the queued frame must never update an absent native view.
  proxy.retained.erase(tag);
  assert(proxy.flush().empty());

  // Given the snapshot advances every frame, so repeated updates should not start over.
  {
    auto lock = manager.lock();
    manager.updateRetainedRoot(first.root, updated);
    assert(manager.getRetainedRoots(1).front() == updated);
    assert(manager.getRetainedProps(updated, props).size() == 1);
    assert(manager.getRetainedProps(first.root, props).empty());
  }

  // Given mixed units and transforms still read native dimensions during an exit.
  css::ViewStylesRepository styles(staticProps, nullptr);
  styles.setLastMountedRoot(removed);
  styles.setRetainedRootsProvider([&manager](SurfaceId surface) { return manager.getRetainedRoots(surface); });

  // When a property asks for its own or its parent's current dimensions.
  {
    auto lock = manager.lock();

    // Then the private snapshot supplies the current geometry after the live tree drops the view.
    assert(styles.getNodeProp(first.indicator, "width").getNumber() == 225);
    assert(styles.getParentNodeProp(first.indicator, "width").getNumber() == 300);
  }

  // When the native exit finishes and the retained view is finally removed.
  {
    auto lock = manager.lock();
    manager.handleNodeRemovals(removed, [](Tag) { return false; });

    // Then its animation and snapshot are released, while the other surface stays intact.
    assert(registry->removed.contains(tag));
    assert(!registry->removed.contains(secondTag));
    assert(manager.getRetainedRoots(1).empty());
  }

  // Given a retained animation is remounted by Strict Mode before native deletion.
  {
    auto lock = manager.lock();
    manager.handleNodeRemovals(first.root, [](Tag) { return false; });
    manager.markNodeAsRemovable(first.indicator);
    manager.handleNodeRemovals(removed, [](Tag) { return true; });
    assert(!manager.getRetainedRoots(1).empty());

    // When React cancels unmount cleanup, then it must stop using the old snapshot.
    manager.unmarkNodeAsRemovable(tag);
    assert(manager.getRetainedRoots(1).empty());
  }

  // Given a second surface shuts down without another mount report.
  {
    auto lock = manager.lock();

    // When the surface is removed, then all of its pending unmounts must be cleaned up.
    manager.removeSurface(11);
    assert(registry->removed.contains(secondTag));
  }
  std::cout << "PASS: exit retention, percentage layout, native mounting updates, opacity, queued frames, relative values, cleanup, multiple surfaces, and remounts\n";
}
