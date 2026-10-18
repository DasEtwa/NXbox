package com.nxhelper.credentials

import android.app.Activity
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import app.tauri.annotation.Command
import app.tauri.annotation.InvokeArg
import app.tauri.annotation.TauriPlugin
import app.tauri.plugin.Invoke
import app.tauri.plugin.JSObject
import app.tauri.plugin.Plugin
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

@InvokeArg
class StoreArgs {
    lateinit var host: String
    var value: String? = null
}

@TauriPlugin
class CredentialsPlugin(private val activity: Activity) : Plugin(activity) {
    @Command
    fun documents(invoke: Invoke) {
        try {
            val args = invoke.parseArgs(StoreArgs::class.java)
            val uri = android.net.Uri.parse(args.host)
            val output = app.tauri.plugin.JSArray()
            val projection =
                arrayOf(
                    android.provider.DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    android.provider.DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                    android.provider.DocumentsContract.Document.COLUMN_MIME_TYPE,
                )
            if (android.provider.DocumentsContract.isTreeUri(uri)) {
                val parent = android.provider.DocumentsContract.getTreeDocumentId(uri)
                val children =
                    android.provider.DocumentsContract.buildChildDocumentsUriUsingTree(uri, parent)
                activity.contentResolver.query(children, projection, null, null, null)?.use { cursor
                    ->
                    while (cursor.moveToNext()) {
                        val name = cursor.getString(0)
                        if (name.endsWith(".nca", true)) {
                            val child =
                                android.provider.DocumentsContract.buildDocumentUriUsingTree(
                                    uri,
                                    cursor.getString(1),
                                )
                            output.put(JSObject().put("path", child.toString()).put("name", name))
                        }
                    }
                }
            } else {
                activity.contentResolver
                    .query(
                        uri,
                        arrayOf(android.provider.OpenableColumns.DISPLAY_NAME),
                        null,
                        null,
                        null,
                    )
                    ?.use { cursor ->
                        if (cursor.moveToFirst())
                            output.put(
                                JSObject()
                                    .put("path", uri.toString())
                                    .put("name", cursor.getString(0))
                            )
                    }
            }
            invoke.resolve(JSObject().put("files", output))
        } catch (error: Exception) {
            invoke.reject(error.message ?: "File metadata unavailable")
        }
    }

    private val alias = "nxbhelper.portal"

    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias, null) as? SecretKey)?.let {
            return it
        }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
            .apply {
                init(
                    KeyGenParameterSpec.Builder(
                            alias,
                            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
                        )
                        .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                        .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                        .build()
                )
            }
            .generateKey()
    }

    @Command
    fun load(invoke: Invoke) =
        run(invoke) { args ->
            val saved = activity.getSharedPreferences(alias, 0).getString(args.host, null)
            val result = JSObject()
            if (saved != null) {
                val bytes = Base64.decode(saved, Base64.NO_WRAP)
                val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                cipher.init(
                    Cipher.DECRYPT_MODE,
                    key(),
                    GCMParameterSpec(128, bytes.copyOfRange(0, 12)),
                )
                result.put(
                    "value",
                    String(cipher.doFinal(bytes.copyOfRange(12, bytes.size)), Charsets.UTF_8),
                )
            }
            result
        }

    @Command
    fun save(invoke: Invoke) =
        run(invoke) { args ->
            val prefs = activity.getSharedPreferences(alias, 0).edit()
            if (args.value == null) prefs.remove(args.host)
            else {
                val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                cipher.init(Cipher.ENCRYPT_MODE, key())
                val encrypted = cipher.iv + cipher.doFinal(args.value!!.toByteArray(Charsets.UTF_8))
                prefs.putString(args.host, Base64.encodeToString(encrypted, Base64.NO_WRAP))
            }
            check(prefs.commit()) { "Credential storage failed" }
            JSObject()
        }

    private fun run(invoke: Invoke, block: (StoreArgs) -> JSObject) {
        try {
            invoke.resolve(block(invoke.parseArgs(StoreArgs::class.java)))
        } catch (error: Exception) {
            invoke.reject(error.message ?: "Credential storage failed")
        }
    }
}
