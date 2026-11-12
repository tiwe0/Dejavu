package cafe.ivory.dejavu.data

import android.net.LocalSocket
import org.json.JSONObject

private val token = "a".repeat(64)
private fun metadata(): JSONObject = JSONObject().put("transport", "unix").put("namespace", "abstract").put("name", "dejavu.router.control").put("token", token)
private fun snapshot(): JSONObject = JSONObject("""{"ok":true,"status":{"pid":42,"hub_connected":true,"plugin_count":1,"pending_jobs":0,"listen_port":39111,"hub_host":"localhost","hub_port":39010,"device_id":"test","token_configured":true,"routes":[{"route":"opaque","app":"example.app","online":true,"configured":false}]}}""")
private inline fun rejected(block: () -> Unit) { check(runCatching(block).isFailure) }

fun main() {
    rejected { RouterEndpoint.parse(metadata().put("transport", "tcp").toString()) }
    rejected { RouterEndpoint.parse(metadata().put("namespace", "filesystem").toString()) }
    rejected { RouterEndpoint.parse(metadata().put("name", "/tmp/untrusted").toString()) }
    rejected { RouterEndpoint.parse(metadata().put("token", "short").toString()) }
    rejected { RouterProtocol.snapshot(JSONObject("""{"ok":false,"error":"invalid_route"}""")) }
    val state = RouterProtocol.snapshot(snapshot())
    check(state.routes.single().route == "opaque" && state.routes.single().online)
    check(!state.routes.single().configured && state.tokenConfigured)
    val service = RouterService(installed = true, running = true, enabled = true)
    val healthy = RouterUiState().readSucceeded(service, state)
    check(healthy.healthKnown && healthy.snapshot?.hubConnected == true)
    val failed = healthy.readFailed()
    check(!failed.healthKnown && failed.snapshot == null && failed.service?.installed == true)
    check(failed.service?.running == true) { "Retain Stop capability while health is unknown" }
    val recovered = failed.readSucceeded(service, state.copy(pid = 43))
    check(recovered.healthKnown && recovered.snapshot?.pid == 43 && recovered.failure == null)

    for (operation in listOf("status", "metadata")) {
        val entered = java.util.concurrent.CountDownLatch(1)
        val released = java.util.concurrent.CountDownLatch(1)
        val cancellable = RouterClient({ error("Read must use cancellable root bridge") }, { _, cancellation ->
            cancellation.withResource({ released.countDown() }) {
                entered.countDown()
                check(released.await(2, java.util.concurrent.TimeUnit.SECONDS))
                ShellResult(0, metadata().toString())
            }
        })
        val worker = kotlin.concurrent.thread {
            rejected { if (operation == "status") cancellable.lifecycle("status") else cancellable.request("status") }
        }
        check(entered.await(1, java.util.concurrent.TimeUnit.SECONDS))
        cancellable.cancelRead()
        worker.join(1000)
        check(!worker.isAlive) { "Hidden page must cancel root $operation discovery" }
    }
    val mutationEntered = java.util.concurrent.CountDownLatch(1)
    val mutationReleased = java.util.concurrent.CountDownLatch(1)
    val mutations = RouterClient({
        mutationEntered.countDown()
        check(mutationReleased.await(2, java.util.concurrent.TimeUnit.SECONDS))
        ShellResult(0, """{"installed":true,"running":false,"enabled":false}""")
    }, { _, _ -> error("Mutation must not use cancellable read bridge") })
    val stopping = kotlin.concurrent.thread { check(!mutations.lifecycle("stop").running) }
    check(mutationEntered.await(1, java.util.concurrent.TimeUnit.SECONDS))
    mutations.cancelRead()
    check(stopping.isAlive) { "Page hiding must not cancel an authorized stop" }
    mutationReleased.countDown()
    stopping.join(1000)
    check(!stopping.isAlive)
    LocalSocket.response = snapshot().toString() + "\n"
    var metadataReads = 0
    val client = RouterClient { command ->
        check(command == "cat /data/adb/dejavu/router/control.json")
        metadataReads++
        ShellResult(0, metadata().toString())
    }
    LocalSocket.onConnect = { check(it.name == "dejavu.router.control") }
    repeat(2) { check(client.request("status").pid == 42) }
    check(metadataReads == 1) { "Polling must not repeatedly invoke su" }
    LocalSocket.requests.forEach {
        val request = JSONObject(it.toString())
        check(request.getString("token") == token && request.getString("op") == "status")
    }
    LocalSocket.uid = 2000
    rejected { client.request("status") }
    check(LocalSocket.requests.last().size() == 0) { "Do not send credentials to a non-root peer" }
    LocalSocket.uid = 0
    LocalSocket.response = "a".repeat(RouterProtocol.MAX_RESPONSE + 1) + "\n"
    rejected { client.request("status") }
    LocalSocket.response = snapshot().toString() // no complete frame
    rejected { client.request("status") }
    LocalSocket.response = snapshot().toString() + "\n"
    LocalSocket.onConnect = { client.cancelRead() }
    rejected { client.request("status") }
    println("Router client regression tests passed")
}
