package android.provider

import android.net.Uri

object DocumentsContract {
    fun isTreeUri(uri: Uri): Boolean = uri.tree
    fun getTreeDocumentId(uri: Uri): String {
        check(uri.tree)
        return uri.documentId
    }
    fun getDocumentId(uri: Uri): String = uri.documentId
    fun buildDocumentUriUsingTree(uri: Uri, id: String): Uri = Uri(uri.treeId, id)
    fun buildChildDocumentsUriUsingTree(uri: Uri, id: String): Uri = Uri(uri.treeId, id, children = true)

    object Document {
        const val COLUMN_DOCUMENT_ID = "document_id"
        const val COLUMN_DISPLAY_NAME = "_display_name"
        const val COLUMN_MIME_TYPE = "mime_type"
        const val MIME_TYPE_DIR = "vnd.android.document/directory"
    }
}
