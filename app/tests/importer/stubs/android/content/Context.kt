package android.content

import android.database.Cursor
import android.net.Uri
import java.io.InputStream

class Context(val contentResolver: ContentResolver) {
    val applicationContext: Context get() = this
}

abstract class ContentResolver {
    abstract fun query(
        uri: Uri,
        projection: Array<String>,
        selection: String?,
        arguments: Array<String>?,
        sort: String?,
    ): Cursor?

    abstract fun openInputStream(uri: Uri): InputStream?
}
