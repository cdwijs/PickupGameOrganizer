package org.pgo.minimal

import java.security.KeyPairGenerator
import java.security.SecureRandom
import java.security.spec.ECGenParameterSpec
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * The same primitives prototype-minimal gets from WebCrypto, from the platform
 * instead: PBKDF2-SHA256 at 310 000 iterations, AES-256-GCM with the nonce
 * prefixed to the ciphertext, and an ECDSA P-256 keypair for the group1
 * section.
 *
 * The parameters are identical to the web and WebAssembly prototypes on
 * purpose, so a vault copied out of any of them opens in the others.
 */
object Crypto {

    const val ITERATIONS = 310_000
    const val SALT_BYTES = 16
    const val NONCE_BYTES = 12
    const val TAG_BITS = 128
    const val KDF_NAME = "PBKDF2-SHA256"

    private val rng = SecureRandom()

    fun randomBytes(n: Int): ByteArray = ByteArray(n).also { rng.nextBytes(it) }

    /**
     * PBKDF2-HMAC-SHA256, written out rather than taken from
     * `SecretKeyFactory`/`PBEKeySpec`.
     *
     * The reason is interoperability. `PBEKeySpec` takes a `CharArray`, and how
     * those characters become bytes has historically differed between
     * providers — the PKCS#5 algorithms use the low byte of each character,
     * while the SHA-2 variants generally use UTF-8. WebCrypto is unambiguous:
     * it hashes the UTF-8 bytes. Doing the derivation over
     * `password.toByteArray(UTF-8)` here removes the question entirely, and
     * costs about twenty lines.
     *
     * dkLen is fixed at 32, so there is exactly one block and no outer loop.
     */
    fun deriveKey(password: String, salt: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        val keySpec = SecretKeySpec(password.toByteArray(Charsets.UTF_8), "HmacSHA256")
        mac.init(keySpec)

        // U1 = PRF(password, salt || INT_BE(1))
        mac.update(salt)
        mac.update(byteArrayOf(0, 0, 0, 1))
        var u = mac.doFinal()
        val t = u.copyOf()

        for (i in 1 until ITERATIONS) {
            mac.reset()
            u = mac.doFinal(u)
            for (j in t.indices) t[j] = (t[j].toInt() xor u[j].toInt()).toByte()
        }
        return t
    }

    /** Returns nonce || ciphertext || tag, the envelope the other prototypes store. */
    fun seal(key: ByteArray, nonce: ByteArray, plaintext: ByteArray): ByteArray {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(
            Cipher.ENCRYPT_MODE,
            SecretKeySpec(key, "AES"),
            GCMParameterSpec(TAG_BITS, nonce)
        )
        val body = cipher.doFinal(plaintext)   // ciphertext with the tag appended
        return nonce + body
    }

    /** Null when the tag does not verify, which is what a wrong password looks like. */
    fun open(key: ByteArray, envelope: ByteArray): ByteArray? {
        if (envelope.size < NONCE_BYTES + TAG_BITS / 8) return null
        return try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(
                Cipher.DECRYPT_MODE,
                SecretKeySpec(key, "AES"),
                GCMParameterSpec(TAG_BITS, envelope, 0, NONCE_BYTES)
            )
            cipher.doFinal(envelope, NONCE_BYTES, envelope.size - NONCE_BYTES)
        } catch (e: Exception) {
            // AEADBadTagException for a wrong key, and anything else here means
            // the blob is not usable either way.
            null
        }
    }

    /** The group1 keypair: SPKI for the public half, PKCS#8 for the private one. */
    data class GroupKeys(val spki: ByteArray, val pkcs8: ByteArray)

    fun generateGroupKeys(): GroupKeys {
        val gen = KeyPairGenerator.getInstance("EC")
        gen.initialize(ECGenParameterSpec("secp256r1"), rng)
        val pair = gen.generateKeyPair()
        // getEncoded() is X.509/SPKI for the public key and PKCS#8 for the
        // private one — the same two encodings WebCrypto exports.
        return GroupKeys(pair.public.encoded, pair.private.encoded)
    }

    private const val HEXD = "0123456789abcdef"

    fun toHex(bytes: ByteArray): String {
        val sb = StringBuilder(bytes.size * 2)
        for (b in bytes) {
            val v = b.toInt() and 0xff
            sb.append(HEXD[v ushr 4]).append(HEXD[v and 15])
        }
        return sb.toString()
    }

    /** Null rather than an exception: pasted text is user input, not a bug. */
    fun fromHex(hex: String): ByteArray? {
        val clean = hex.trim().filter { !it.isWhitespace() }
        if (clean.length % 2 != 0) return null
        val out = ByteArray(clean.length / 2)
        for (i in out.indices) {
            val hi = Character.digit(clean[i * 2], 16)
            val lo = Character.digit(clean[i * 2 + 1], 16)
            if (hi < 0 || lo < 0) return null
            out[i] = ((hi shl 4) or lo).toByte()
        }
        return out
    }
}
