package org.pgo.minimal

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.SharedPreferences
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.View
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.pgo.minimal.databinding.ActivityMainBinding
import org.pgo.minimal.databinding.GameCardBinding

/**
 * prototype-minimal as an Android app.
 *
 * Same three ideas: a vault of password-encrypted blobs where the username
 * lives inside the ciphertext, a roster that is parsed and rewritten around the
 * signed-in user, and a diagnostic panel that folds away.
 *
 * What is different is what the platform gives instead of the browser:
 * SharedPreferences instead of localStorage, the Android clipboard instead of
 * the async Clipboard API, and autofill instead of the Credential Management
 * API — which is why there is no silent re-unlock on launch. See the README.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var b: ActivityMainBinding
    private lateinit var prefs: SharedPreferences

    private var records: List<Record> = emptyList()
    private var username = ""
    private var userId = ""
    private var userData = ""          // decrypted plaintext, memory only
    private var showDebug = false
    private var rawRoster = ""
    private var parsed: Roster.Parsed = Roster.parse("")
    private var rosterTime = ""
    private var unlockTargetId = ""    // set when the form is opened to unlock
    private var busy = false
    private var suppressVaultWatcher = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        b = ActivityMainBinding.inflate(layoutInflater)
        setContentView(b.root)

        prefs = getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        records = Vault.parse(prefs.getString(KEY_VAULT, null))
        showDebug = prefs.getBoolean(KEY_DEBUG, false)
        restoreSession()

        wireUp()
        render()
    }

    // ---- session ---------------------------------------------------------

    /**
     * A restored session carries the username and the blob id, never the
     * password: re-deriving the key needs the password again, so a restored
     * session has no plaintext until Unlock.
     */
    private fun restoreSession() {
        val raw = prefs.getString(KEY_SESSION, null) ?: return
        try {
            val obj = JSONObject(raw)
            val id = obj.optString("id")
            val user = obj.optString("username")
            if (id.isEmpty() || user.isEmpty()) return
            if (records.none { it.id == id }) {
                prefs.edit().remove(KEY_SESSION).apply()
                return
            }
            userId = id
            username = user
            b.usernameInput.setText(user)
        } catch (e: Exception) {
            prefs.edit().remove(KEY_SESSION).apply()
        }
    }

    private fun saveSession() {
        prefs.edit().putString(
            KEY_SESSION,
            JSONObject().put("id", userId).put("username", username).toString()
        ).apply()
    }

    /** Drop the signed-in state without touching the vault. */
    private fun signOutLocal() {
        username = ""
        userId = ""
        userData = ""
        b.passwordInput.setText("")
        prefs.edit().remove(KEY_SESSION).apply()
    }

    private fun saveVault() {
        prefs.edit().putString(KEY_VAULT, Vault.serialize(records)).apply()
    }

    // ---- wiring ----------------------------------------------------------

    private fun wireUp() {
        b.accountBtn.setOnClickListener {
            if (username.isNotEmpty()) {
                signOutLocal()
                render()
            } else {
                unlockTargetId = ""
                showSignin(focusPassword = false)
            }
        }

        b.debugBtn.setOnClickListener {
            showDebug = !showDebug
            prefs.edit().putBoolean(KEY_DEBUG, showDebug).apply()
            render()
        }

        b.unlockBtn.setOnClickListener {
            b.usernameInput.setText(username)
            b.passwordInput.setText("")
            unlockTargetId = userId
            showSignin(focusPassword = true)
        }

        b.signinCancel.setOnClickListener {
            b.usernameInput.setText(username)
            b.passwordInput.setText("")
            unlockTargetId = ""
            showMain()
        }

        b.signinSubmit.setOnClickListener { submitSignin() }

        b.vaultCopy.setOnClickListener {
            copyToClipboard(b.vaultBox.text.toString())
            flash(b.vaultStatus, "copied")
        }
        b.vaultPaste.setOnClickListener {
            val text = readClipboard() ?: return@setOnClickListener
            suppressVaultWatcher = true
            b.vaultBox.setText(text)
            suppressVaultWatcher = false
            ingest(text)
        }
        b.vaultBox.addTextChangedListener(object : TextWatcher {
            override fun afterTextChanged(s: Editable?) {
                if (!suppressVaultWatcher) ingest(s?.toString() ?: "")
            }
            override fun beforeTextChanged(s: CharSequence?, a: Int, c: Int, d: Int) = Unit
            override fun onTextChanged(s: CharSequence?, a: Int, c: Int, d: Int) = Unit
        })

        b.deleteUser.setOnClickListener {
            if (userId.isEmpty()) return@setOnClickListener
            confirm(
                "Delete this user? The encrypted blob is removed from this device "
                    + "and cannot be recovered without a copy."
            ) {
                records = records.filterNot { it.id == userId }
                saveVault()
                signOutLocal()
                render()
            }
        }

        b.deleteAll.setOnClickListener {
            val n = records.size
            if (n == 0) return@setOnClickListener
            confirm(
                "Delete all $n user${if (n == 1) "" else "s"}? Every encrypted blob on "
                    + "this device is removed and cannot be recovered without a copy."
            ) {
                records = emptyList()
                saveVault()
                signOutLocal()
                render()
            }
        }

        b.pasteIn.addTextChangedListener(object : TextWatcher {
            override fun afterTextChanged(s: Editable?) {
                val text = s?.toString() ?: ""
                if (text == rawRoster) return
                rawRoster = text
                reparse()
                render()
            }
            override fun beforeTextChanged(s: CharSequence?, a: Int, c: Int, d: Int) = Unit
            override fun onTextChanged(s: CharSequence?, a: Int, c: Int, d: Int) = Unit
        })

        b.pasteBtn.setOnClickListener {
            val text = readClipboard() ?: return@setOnClickListener
            b.pasteIn.setText(text)     // the watcher picks it up
        }
        b.clearBtn.setOnClickListener { b.pasteIn.setText("") }
        b.copyBtn.setOnClickListener {
            copyToClipboard(b.pasteOut.text.toString())
            flash(b.outStatus, "copied")
        }

        b.card0.cardToggle.setOnClickListener { toggleBlock(0) }
        b.card1.cardToggle.setOnClickListener { toggleBlock(1) }
    }

    // ---- sign in ---------------------------------------------------------

    private fun showSignin(focusPassword: Boolean) {
        b.mainView.visibility = View.GONE
        b.signinView.visibility = View.VISIBLE
        (if (focusPassword) b.passwordInput else b.usernameInput).requestFocus()
    }

    private fun showMain() {
        b.signinView.visibility = View.GONE
        b.mainView.visibility = View.VISIBLE
    }

    private fun setBusy(value: Boolean) {
        busy = value
        b.signinSubmit.isEnabled = !value
        b.signinSubmit.text = if (value) getString(R.string.working) else getString(R.string.sign_in)
        b.usernameInput.isEnabled = !value
        b.passwordInput.isEnabled = !value
    }

    private fun submitSignin() {
        if (busy) return
        val user = b.usernameInput.text.toString().trim()
        val pass = b.passwordInput.text.toString()
        if (user.isEmpty()) {
            alert("Username can not be empty."); b.usernameInput.requestFocus(); return
        }
        if (pass.isEmpty()) {
            alert("Password can not be empty."); b.passwordInput.requestFocus(); return
        }

        // 310 000 iterations, once per stored blob. Off the main thread, or the
        // UI freezes for as long as it takes.
        val target = unlockTargetId
        if (target.isNotEmpty() && records.none { it.id == target }) {
            // Deleted meanwhile. The session points at nothing, so drop it
            // rather than blame the password.
            signOutLocal(); showMain(); render()
            alert("That user was deleted on this device.")
            return
        }

        setBusy(true)
        lifecycleScope.launch {
            val hit: Unlocked? = withContext(Dispatchers.Default) {
                if (target.isNotEmpty()) Vault.unlockById(records, target, pass)
                else Vault.signIn(records, user, pass)
            }
            setBusy(false)

            if (hit != null) {
                adopt(hit)
                unlockTargetId = ""
                return@launch
            }

            // An unlock knows the account exists, so it is plainly the password;
            // a sign-in cannot tell an unknown user from a wrong password, and
            // must not say.
            if (target.isNotEmpty()) {
                alert(getString(R.string.wrong_password))
                return@launch
            }
            alert(getString(R.string.wrong_credentials)) {
                confirm("Create a new user \"$user\" with this password?") {
                    setBusy(true)
                    lifecycleScope.launch {
                        val made = withContext(Dispatchers.Default) { Vault.create(records, user, pass) }
                        records = made.first
                        saveVault()
                        setBusy(false)
                        adopt(made.second)
                    }
                }
            }
        }
    }

    /** Take the unlocked account as the current session. */
    private fun adopt(hit: Unlocked) {
        username = hit.username        // the stored spelling wins
        userId = hit.record.id
        userData = hit.text
        saveSession()
        b.passwordInput.setText("")
        showMain()
        reparse()
        render()
    }

    // ---- roster ----------------------------------------------------------

    private fun reparse() {
        parsed = Roster.parse(rawRoster)
        Roster.normalizeUserSlots(parsed, username)
        rosterTime = Roster.extractTime(rawRoster)
    }

    private fun toggleBlock(index: Int) {
        if (username.isEmpty()) return
        val block = parsed.blocks.getOrNull(index) ?: return
        if (Roster.isUserIn(block, username)) Roster.removeUser(block, username)
        else Roster.addUser(block, username)
        render()
    }

    // ---- render ----------------------------------------------------------

    private fun render() {
        // account
        if (username.isNotEmpty()) {
            b.accountStatus.text = "signed in as $username"
            b.accountStatus.setTextColor(ContextCompat.getColor(this, R.color.ok))
            b.accountBtn.text = getString(R.string.sign_out)
        } else {
            b.accountStatus.text = getString(R.string.not_signed_in)
            b.accountStatus.setTextColor(ContextCompat.getColor(this, R.color.muted))
            b.accountBtn.text = getString(R.string.sign_in)
        }

        b.debugPanel.visibility = if (showDebug) View.VISIBLE else View.GONE

        // vault
        if (!b.vaultBox.hasFocus()) {
            suppressVaultWatcher = true
            b.vaultBox.setText(if (records.isEmpty()) "" else Vault.prettyPrint(records))
            suppressVaultWatcher = false
        }
        b.vaultStatus.text = when {
            records.isEmpty() -> getString(R.string.empty)
            userData.isNotEmpty() -> "${blobCount()} · unlocked"
            else -> blobCount()
        }
        b.deleteUser.isEnabled = userId.isNotEmpty()
        b.deleteAll.isEnabled = records.isNotEmpty()

        // decrypted
        val locked = username.isNotEmpty() && userData.isEmpty()
        b.plainBox.text = userData.ifEmpty {
            if (locked) getString(R.string.plain_locked) else getString(R.string.plain_placeholder)
        }
        b.plainStatus.text = when {
            userData.isNotEmpty() -> "${userData.length} chars"
            username.isNotEmpty() -> "locked"
            else -> getString(R.string.dash)
        }
        b.unlockBtn.visibility = if (locked) View.VISIBLE else View.GONE

        // roster
        if (rawRoster.isEmpty()) {
            b.parseStatus.text = getString(R.string.empty)
            b.outStatus.text = getString(R.string.dash)
            b.pasteOut.text = getString(R.string.out_placeholder)
        } else {
            val n = parsed.blocks.size
            b.parseStatus.text = "$n date block${if (n == 1) "" else "s"}"
            val out = Roster.render(parsed)
            b.pasteOut.text = out
            b.outStatus.text = "${out.length} chars"
        }

        renderCard(b.card0, 0)
        renderCard(b.card1, 1)
    }

    private fun blobCount(): String {
        val n = records.size
        return "$n blob${if (n == 1) "" else "s"}"
    }

    private fun renderCard(card: GameCardBinding, index: Int) {
        val block = parsed.blocks.getOrNull(index)
        if (block == null) {
            card.cardWhen.text = getString(R.string.dash)
            card.cardCount.text = getString(R.string.zero)
            card.cardToggle.text = getString(R.string.not_going)
            card.cardToggle.isEnabled = false
            return
        }
        val wd = Roster.shortWeekday(block)
        card.cardWhen.text = if (rosterTime.isNotEmpty()) "$wd · $rosterTime" else wd
        card.cardCount.text = Roster.countFilled(block).toString()
        val going = Roster.isUserIn(block, username)
        card.cardToggle.text = getString(if (going) R.string.going else R.string.not_going)
        card.cardToggle.isEnabled = username.isNotEmpty()
        card.cardWhen.setTextColor(
            ContextCompat.getColor(this, if (going) R.color.ok else R.color.accent)
        )
    }

    // ---- vault paste -----------------------------------------------------

    private fun ingest(text: String) {
        if (text.isBlank()) return
        try {
            val result = Vault.ingest(records, text)
            records = result.records
            saveVault()
            render()
            if (result.added > 0 || result.replaced > 0) {
                flash(b.vaultStatus, "+${result.added} ~${result.replaced}")
            }
        } catch (e: BadRecord) {
            b.vaultStatus.text = "bad paste"
        }
    }

    // ---- little helpers --------------------------------------------------

    private fun copyToClipboard(text: String) {
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("roster", text))
    }

    private fun readClipboard(): String? {
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val clip = cm.primaryClip
        if (clip == null || clip.itemCount == 0) {
            alert("The clipboard is empty.")
            return null
        }
        return clip.getItemAt(0).coerceToText(this).toString()
    }

    private fun flash(view: android.widget.TextView, message: String) {
        view.text = message
        view.postDelayed({ render() }, 1200)
    }

    private fun alert(message: String, onDismiss: (() -> Unit)? = null) {
        AlertDialog.Builder(this)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok) { d, _ -> d.dismiss() }
            .setOnDismissListener { onDismiss?.invoke() }
            .show()
    }

    private fun confirm(message: String, onYes: () -> Unit) {
        AlertDialog.Builder(this)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok) { _, _ -> onYes() }
            .setNegativeButton(android.R.string.cancel) { d, _ -> d.dismiss() }
            .show()
    }

    companion object {
        private const val PREFS = "prototype-minimal-apk"
        private const val KEY_VAULT = "vault:v1"
        private const val KEY_SESSION = "session:v1"
        private const val KEY_DEBUG = "debug:v1"
    }
}
