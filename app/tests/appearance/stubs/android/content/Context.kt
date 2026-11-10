package android.content

class Context(private val stores: MutableMap<String, SharedPreferences> = mutableMapOf()) {
    val applicationContext: Context get() = this
    fun getSharedPreferences(name: String, mode: Int): SharedPreferences {
        check(mode == MODE_PRIVATE)
        return stores.getOrPut(name) { SharedPreferences() }
    }
    fun recreate() = Context(stores)
    companion object { const val MODE_PRIVATE = 0 }
}

class SharedPreferences {
    private val values = mutableMapOf<String, String?>()
    fun getString(key: String, fallback: String?): String? = values[key] ?: fallback
    fun edit() = Editor()
    inner class Editor {
        private val pending = mutableMapOf<String, String?>()
        fun putString(key: String, value: String?): Editor = apply { pending[key] = value }
        fun apply() { values.putAll(pending) }
    }
}
