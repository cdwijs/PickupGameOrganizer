package org.pgo.minimal

import org.json.JSONArray
import org.json.JSONObject

/**
 * The vault: one AES-GCM blob per user, and the rules for reading, merging and
 * writing them. The stored shape is byte-identical to prototype-minimal's, so
 * a vault copied out of the browser opens here and the other way round.
 *
 * The username lives *inside* the ciphertext. Nothing stored in the clear says
 * who the blobs belong to — only how many there are.
 */

const val READABLE_PREFIX = "Readable: "
const val USER_DATA_BODY = "This is a placeholder for the user data"
const val GROUP_SECTION = "group1"
const val GROUP_KEY_LABEL = "ECDSA P-256"

/** One entry. `id` is a public label so the session and delete buttons can point at one blob. */
data class Record(val id: String, val salt: ByteArray, val data: ByteArray) {
    fun toJson(): JSONObject = JSONObject().apply {
        put("v", 2)
        put("kdf", Crypto.KDF_NAME)
        put("iterations", Crypto.ITERATIONS)
        put("id", id)
        put("salt", Crypto.toHex(salt))
        put("data", Crypto.toHex(data))
    }

    /** Same blob under the same id means a paste has nothing to say. */
    fun sameContentAs(other: Record): Boolean =
        salt.contentEquals(other.salt) && data.contentEquals(other.data)
}

/** What a successful unlock produces. */
data class Unlocked(val record: Record, val username: String, val text: String)

class BadRecord(message: String) : Exception(message)

object Vault {

    // ---- plaintext -------------------------------------------------------

    fun makePlaintext(username: String, keys: Crypto.GroupKeys): String = buildString {
        append(READABLE_PREFIX).append(username).append('\n')
        append(USER_DATA_BODY).append("\n\n")
        append('[').append(GROUP_SECTION).append("]\n")
        append("alg: ").append(GROUP_KEY_LABEL).append('\n')
        append("public: ").append(Crypto.toHex(keys.spki)).append('\n')
        append("private: ").append(Crypto.toHex(keys.pkcs8))
    }

    /**
     * The username on the marker line, or null when this is not one of ours.
     * A blob that decrypts but does not start with the marker means the key was
     * wrong in a way GCM happened not to catch — vanishingly unlikely, but the
     * check is free.
     */
    fun usernameOf(plaintext: String): String? {
        if (!plaintext.startsWith(READABLE_PREFIX)) return null
        val end = plaintext.indexOf('\n').let { if (it < 0) plaintext.length else it }
        val name = plaintext.substring(READABLE_PREFIX.length, end).trim()
        return name.ifEmpty { null }
    }

    // ---- reading and writing the stored array ----------------------------

    /** Anything unparseable is dropped rather than thrown: one bad entry must not lock the app out. */
    fun parse(json: String?): List<Record> {
        if (json.isNullOrBlank()) return emptyList()
        val arr = try { JSONArray(json) } catch (e: Exception) { return emptyList() }
        val out = ArrayList<Record>(arr.length())
        for (i in 0 until arr.length()) {
            val obj = arr.optJSONObject(i) ?: continue
            val rec = try { recordFrom(obj, requireId = true) } catch (e: BadRecord) { continue }
            out.add(rec)
        }
        return out
    }

    fun serialize(records: List<Record>): String {
        val arr = JSONArray()
        for (r in records) arr.put(r.toJson())
        return arr.toString()
    }

    /** What the user-data box shows: the same array, indented so it can be read. */
    fun prettyPrint(records: List<Record>): String {
        if (records.isEmpty()) return ""
        val arr = JSONArray()
        for (r in records) arr.put(r.toJson())
        return arr.toString(2)
    }

    /**
     * Validate one entry. A mismatched KDF or iteration count is named rather
     * than left to surface later as a failed decryption, which would read as
     * "wrong password" and be worse.
     */
    private fun recordFrom(obj: JSONObject, requireId: Boolean): Record {
        val kdf = obj.optString("kdf", "")
        if (kdf.isNotEmpty() && kdf != Crypto.KDF_NAME) throw BadRecord("unsupported kdf \"$kdf\"")
        val iterations = obj.optInt("iterations", Crypto.ITERATIONS)
        if (iterations != Crypto.ITERATIONS) throw BadRecord("unsupported iteration count $iterations")

        val saltHex = obj.optString("salt", obj.optString("saltHex", ""))
        val dataHex = obj.optString("data", obj.optString("dataHex", ""))
        val salt = Crypto.fromHex(saltHex) ?: throw BadRecord("salt is not hex")
        val data = Crypto.fromHex(dataHex) ?: throw BadRecord("encrypted data is not hex")
        if (salt.size != Crypto.SALT_BYTES) throw BadRecord("salt must be ${Crypto.SALT_BYTES} bytes")
        if (data.size < Crypto.NONCE_BYTES + Crypto.TAG_BITS / 8) throw BadRecord("encrypted data is too short")

        val rawId = obj.optString("id", "")
        val id = if (rawId.matches(Regex("^[0-9a-fA-F]{4,64}$"))) rawId.lowercase() else ""
        if (id.isEmpty() && requireId) throw BadRecord("no usable id")
        return Record(id, salt, data)
    }

    fun newId(): String = Crypto.toHex(Crypto.randomBytes(8))

    // ---- the operations the UI asks for ----------------------------------

    /**
     * Create: fresh salt, key derived from it and the password, then the
     * username, the body and a brand-new keypair sealed under that key.
     *
     * The plaintext comes back with the record because the keypair is random —
     * rebuilding the text afterwards would invent a different one than the blob
     * holds.
     */
    fun create(records: List<Record>, username: String, password: String): Pair<List<Record>, Unlocked> {
        val salt = Crypto.randomBytes(Crypto.SALT_BYTES)
        val nonce = Crypto.randomBytes(Crypto.NONCE_BYTES)
        val key = Crypto.deriveKey(password, salt)
        val text = makePlaintext(username, Crypto.generateGroupKeys())
        val envelope = Crypto.seal(key, nonce, text.toByteArray(Charsets.UTF_8))
        val rec = Record(newId(), salt, envelope)
        return Pair(records + rec, Unlocked(rec, username, text))
    }

    private fun unlock(rec: Record, password: String): Unlocked? {
        val key = Crypto.deriveKey(password, rec.salt)
        val plain = Crypto.open(key, rec.data) ?: return null
        val text = String(plain, Charsets.UTF_8)
        val user = usernameOf(text) ?: return null
        return Unlocked(rec, user, text)
    }

    /**
     * Sign in: try every blob with this password and keep the one whose
     * encrypted username matches. One key derivation per stored blob — the
     * username is inside the ciphertext, so there is nothing public to look up
     * on. The match ignores case and the *stored* spelling wins.
     */
    fun signIn(records: List<Record>, username: String, password: String): Unlocked? {
        val wanted = username.trim().lowercase()
        for (rec in records) {
            val hit = unlock(rec, password) ?: continue
            if (hit.username.lowercase() == wanted) return hit
        }
        return null
    }

    /** Open one named blob, for a session that knows who it is but has no key. */
    fun unlockById(records: List<Record>, id: String, password: String): Unlocked? =
        records.firstOrNull { it.id == id }?.let { unlock(it, password) }

    data class MergeResult(val records: List<Record>, val added: Int, val replaced: Int)

    /**
     * Merge pasted records: the same id replaces (the same account,
     * re-encrypted), an identical salt and blob is a no-op, anything else is
     * added. Accepts a single record object as readily as an array.
     */
    fun ingest(current: List<Record>, text: String): MergeResult {
        val trimmed = text.trim()
        if (trimmed.isEmpty()) throw BadRecord("nothing pasted")

        val incoming = ArrayList<Record>()
        try {
            if (trimmed.startsWith("[")) {
                val arr = JSONArray(trimmed)
                if (arr.length() == 0) throw BadRecord("no records in there")
                for (i in 0 until arr.length()) {
                    val obj = arr.optJSONObject(i) ?: throw BadRecord("not a record object")
                    incoming.add(recordFrom(obj, requireId = false))
                }
            } else {
                incoming.add(recordFrom(JSONObject(trimmed), requireId = false))
            }
        } catch (e: BadRecord) {
            throw e
        } catch (e: Exception) {
            throw BadRecord("not valid JSON")
        }

        val out = ArrayList(current)
        var added = 0
        var replaced = 0
        for (raw in incoming) {
            val rec = if (raw.id.isEmpty()) raw.copy(id = newId()) else raw
            val at = out.indexOfFirst { it.id == rec.id }
            when {
                at < 0 -> { out.add(rec); added++ }
                out[at].sameContentAs(rec) -> Unit          // identical pair, nothing to do
                else -> { out[at] = rec; replaced++ }
            }
        }
        return MergeResult(out, added, replaced)
    }
}
