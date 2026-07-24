package org.pgo.artip2p

/** JNI bridge to the Rust Arti core (libarti_p2p_android.so). */
object Native {
    init {
        System.loadLibrary("arti_p2p_android")
    }

    /** Set the Arti state/cache base directory (use the app's filesDir). */
    external fun nativeInit(dir: String)

    /** Empty fields => host a new onion service; filled => dial that peer. */
    external fun nativeConnect(onion: String, libp2p: String)

    /** Start streaming an increasing number to the peer every second. */
    external fun nativeStartCounter()

    /** Drain queued events. Returns '\n'-separated `TAGBODY` lines. */
    external fun nativePoll(): String
}
