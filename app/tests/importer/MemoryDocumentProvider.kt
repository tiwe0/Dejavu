package cafe.ivory.dejavu.data

import android.content.ContentResolver
import android.database.Cursor
import android.net.Uri
import android.provider.DocumentsContract
import java.io.ByteArrayInputStream
import java.io.InputStream

class MemoryDocumentProvider : ContentResolver() {
    private data class Node(
        val id: String,
        val name: String,
        val data: ByteArray?,
        val children: MutableMap<String, Node> = linkedMapOf(),
    )

    private val documents = mutableMapOf<Pair<String, String>, Node>()
    private var nextId = 0

    fun tree(files: Map<String, ByteArray>): Uri {
        val treeId = "tree-${++nextId}"
        fun node(name: String, bytes: ByteArray?): Node {
            return Node("opaque-${++nextId}", name, bytes).also { documents[treeId to it.id] = it }
        }
        val root = node("selected folder", null)
        files.forEach { (path, bytes) ->
            val parts = path.split('/')
            var parent = root
            parts.dropLast(1).forEach { name ->
                parent = parent.children.getOrPut(name) { node(name, null) }
                check(parent.data == null)
            }
            check(parts.last() !in parent.children)
            parent.children[parts.last()] = node(parts.last(), bytes.copyOf())
        }
        return Uri(treeId, root.id, tree = true)
    }

    override fun query(
        uri: Uri,
        projection: Array<String>,
        selection: String?,
        arguments: Array<String>?,
        sort: String?,
    ): Cursor {
        check(uri.children && selection == null && arguments == null && sort == null)
        val parent = documents.getValue(uri.treeId to uri.documentId)
        check(parent.data == null)
        val rows = parent.children.values.map { child ->
            projection.map { column ->
                when (column) {
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID -> child.id
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME -> child.name
                    DocumentsContract.Document.COLUMN_MIME_TYPE -> if (child.data == null) {
                        DocumentsContract.Document.MIME_TYPE_DIR
                    } else "text/plain"
                    else -> error("Unexpected provider column: $column")
                }
            }
        }
        return object : Cursor {
            var index = -1
            var closed = false
            override fun moveToNext(): Boolean {
                check(!closed)
                return ++index < rows.size
            }
            override fun getString(index: Int): String {
                check(!closed)
                return rows[this.index][index]
            }
            override fun close() { closed = true }
        }
    }

    override fun openInputStream(uri: Uri): InputStream {
        check(!uri.children)
        return ByteArrayInputStream(documents.getValue(uri.treeId to uri.documentId).data
            ?: error("Cannot read a directory as a source file"))
    }
}
