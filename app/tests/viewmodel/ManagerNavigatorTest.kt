package cafe.ivory.dejavu.data

fun main() {
    // 1. Overview -> PluginDetail -> pop returns directly to Overview (not Plugins).
    val nav = ManagerBackStack()
    check(nav.current == ManagerRoute.Overview && !nav.canPop)
    nav.push(ManagerRoute.PluginDetail("demo"))
    check(nav.current == ManagerRoute.PluginDetail("demo", PluginDetailTab.Control))
    check(nav.previous == ManagerRoute.Overview && nav.canPop)

    // 2. Switching Control / Scope / Info tabs preserves depth=1 and destinationKey so Back pops directly to Overview.
    val controlKey = nav.current.destinationKey
    nav.selectPluginTab(PluginDetailTab.Scope)
    check(nav.current == ManagerRoute.PluginDetail("demo", PluginDetailTab.Scope))
    check(nav.current.destinationKey == controlKey)
    nav.selectPluginTab(PluginDetailTab.Info)
    check(nav.current == ManagerRoute.PluginDetail("demo", PluginDetailTab.Info))
    check(nav.stack.size == 2)

    check(nav.pop(fromGesture = false))
    check(nav.current == ManagerRoute.Overview && !nav.canPop)
    check(!nav.consumeSkipPageTransition())

    // 3. encode / decode round-trip preserves root tab, subpage stack, and PluginDetail tab.
    nav.selectRoot(ManagerRoute.Configuration)
    nav.push(ManagerRoute.Router)
    nav.push(ManagerRoute.PluginDetail("my:plugin", PluginDetailTab.Scope))
    val encoded = nav.encode()
    val restored = ManagerBackStack.decode(encoded)
    check(restored.stack == nav.stack)
    check(restored.current == ManagerRoute.PluginDetail("my:plugin", PluginDetailTab.Scope))
    check(restored.previous == ManagerRoute.Router)

    // 4. consumeSkipPageTransition() is strictly one-shot and cleared on push/selectRoot.
    check(restored.pop(fromGesture = true))
    check(restored.consumeSkipPageTransition())
    check(!restored.consumeSkipPageTransition())

    check(restored.pop(fromGesture = true))
    restored.push(ManagerRoute.Logs)
    check(!restored.consumeSkipPageTransition())

    // 5. Decoding empty or corrupt tokens falls back cleanly to Overview.
    val fallback = ManagerBackStack.decode(listOf("unknown-token"))
    check(fallback.stack == listOf(ManagerRoute.Overview))

    println("OK: ManagerBackStack navigation, sibling tab pop, encode/decode round-trip, and one-shot skip transition")
}
