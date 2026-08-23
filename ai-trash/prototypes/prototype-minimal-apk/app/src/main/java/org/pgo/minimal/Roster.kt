package org.pgo.minimal

import java.time.LocalDate

/**
 * The roster parser and rewriter, ported from prototype-minimal.
 *
 * Kotlin has regular expressions, so unlike the C version this can keep the
 * original patterns almost verbatim — which is the point: the grammar is the
 * behaviour, and restating it in a different shape is how ports drift.
 *
 *   🗓️ Friday 07.08.2026     a date line: emoji, optional weekday, dd.mm.yyyy
 *   01. Alice                a player line: number, dot, optional name
 *   03.                      an empty slot
 *
 * Everything else is carried through untouched.
 */
object Roster {

    const val APP_SUFFIX = " (app)"

    // The leading emoji sometimes carries a variation selector (U+FE0F), so it
    // is matched loosely.
    private val DATE_RE = Regex("""^\s*🗓️?\s*([A-Za-z]+)?\s*(\d{1,2}\.\d{1,2}\.\d{2,4})\s*$""")
    // Numbers are usually zero-padded but that is not required, and the name
    // may be empty for an unfilled slot.
    private val PLAYER_RE = Regex("""^\s*(\d{1,3})\.\s?(.*?)\s*$""")
    private val TIME_RE = Regex("""(\d{1,2})[:.](\d{2})\s*[~\-–—]\s*\d{1,2}[:.]\d{2}""")
    private val DATE_PARTS = Regex("""^(\d{1,2})\.(\d{1,2})\.(\d{2,4})$""")
    private val NAME_PART = Regex("""[^\s'\-]+""")

    private val WEEKDAY_SHORT = listOf("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")

    class Block(
        val weekday: String,
        val date: String,
        var playerStart: Int,
        var playerEnd: Int,
        val players: MutableList<String>,
    )

    class Parsed(val lines: List<String>, val blocks: List<Block>)

    fun parse(text: String): Parsed {
        val lines = text.split(Regex("\r?\n"))
        val blocks = ArrayList<Block>()
        var current: Block? = null

        lines.forEachIndexed { idx, line ->
            val dm = DATE_RE.find(line)
            if (dm != null) {
                current = Block(
                    weekday = dm.groupValues[1],
                    date = dm.groupValues[2],
                    playerStart = -1,
                    playerEnd = -1,
                    players = ArrayList(),
                ).also { blocks.add(it) }
                return@forEachIndexed
            }
            val block = current ?: return@forEachIndexed
            val pm = PLAYER_RE.find(line) ?: return@forEachIndexed
            if (block.playerStart < 0) block.playerStart = idx
            block.playerEnd = idx
            block.players.add(pm.groupValues[2].trim())
        }
        return Parsed(lines, blocks)
    }

    /**
     * Rewrite the original text with the players spliced back in. Untouched
     * lines survive verbatim, so the round trip is stable. Blocks are walked
     * back to front so the earlier indices stay valid.
     */
    fun render(parsed: Parsed): String {
        val out = ArrayList(parsed.lines)
        for (b in parsed.blocks.asReversed()) {
            if (b.playerStart < 0) continue
            val replacement = b.players.mapIndexed { i, name ->
                val n = (i + 1).toString().padStart(2, '0')
                if (name.isEmpty()) "$n. " else "$n. $name"
            }
            // subList().clear() then addAll is Kotlin's splice.
            val range = out.subList(b.playerStart, b.playerEnd + 1)
            range.clear()
            out.addAll(b.playerStart, replacement)
        }
        return out.joinToString("\n")
    }

    /**
     * "cedric" -> "Cedric", "jan-piet" -> "Jan-Piet". Each part keeps the rest
     * of its spelling, so "McKay" survives.
     */
    fun capitalize(name: String): String =
        NAME_PART.replace(name) { m ->
            m.value.replaceFirstChar { it.uppercaseChar() }
        }

    fun displayName(username: String): String = capitalize(username) + APP_SUFFIX

    /** A slot is the user's when it is the bare name or the tagged form, either way ignoring case. */
    fun isSameUser(slot: String, username: String): Boolean {
        val s = slot.lowercase()
        val u = username.lowercase()
        return s == u || s == u + APP_SUFFIX.lowercase()
    }

    fun isUserIn(block: Block, username: String): Boolean =
        username.isNotEmpty() && block.players.any { isSameUser(it, username) }

    fun countFilled(block: Block): Int = block.players.count { it.isNotEmpty() }

    /**
     * Rewrite a bare "<username>" slot to the tagged form so the output always
     * marks the signed-in user. Idempotent.
     */
    fun normalizeUserSlots(parsed: Parsed, username: String) {
        if (username.isEmpty()) return
        val tagged = displayName(username)
        for (b in parsed.blocks) {
            for (i in b.players.indices) {
                if (isSameUser(b.players[i], username) && b.players[i] != tagged) {
                    b.players[i] = tagged
                }
            }
        }
    }

    /** Fill the first gap, or append a slot when they are all taken. */
    fun addUser(block: Block, username: String) {
        val name = displayName(username)
        val gap = block.players.indexOfFirst { it.isEmpty() }
        if (gap >= 0) block.players[gap] = name else block.players.add(name)
    }

    /** Empty the slot but keep it, so the numbering does not shift. */
    fun removeUser(block: Block, username: String) {
        for (i in block.players.indices) {
            if (isSameUser(block.players[i], username)) block.players[i] = ""
        }
    }

    /**
     * Prefer the weekday written in the roster; fall back to computing it from
     * the date, which is what the JS does with `new Date(...)`.
     */
    fun shortWeekday(block: Block): String {
        if (block.weekday.isNotEmpty()) return block.weekday.take(3)
        val m = DATE_PARTS.find(block.date) ?: return ""
        val (d, mo, rawYear) = m.destructured
        val year = if (rawYear.length == 2) "20$rawYear" else rawYear
        return try {
            // DayOfWeek runs Monday..Sunday, which is why the table starts on Monday.
            WEEKDAY_SHORT[LocalDate.of(year.toInt(), mo.toInt(), d.toInt()).dayOfWeek.value - 1]
        } catch (e: Exception) {
            ""
        }
    }

    /** Lift the game time out of the preamble: "🕖 19.00 ~ 21:00" -> "19:00". */
    fun extractTime(text: String): String {
        val m = TIME_RE.find(text) ?: return ""
        return "${m.groupValues[1]}:${m.groupValues[2]}"
    }
}
