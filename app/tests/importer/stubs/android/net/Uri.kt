package android.net

/** Opaque provider document IDs deliberately differ from resource paths. */
data class Uri(
    val treeId: String,
    val documentId: String,
    val tree: Boolean = false,
    val children: Boolean = false,
)
