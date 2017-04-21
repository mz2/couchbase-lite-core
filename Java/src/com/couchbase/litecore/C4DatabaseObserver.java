package com.couchbase.litecore;


import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

public class C4DatabaseObserver {
    private static final String TAG = C4DatabaseObserver.class.getSimpleName();

    //-------------------------------------------------------------------------
    // Static Variables
    //-------------------------------------------------------------------------
    // Long: handle of C4DatabaseObserver native address
    // C4DatabaseObserver: Java class holds handle
    private static Map<Long, C4DatabaseObserver> reverseLookupTable
            = Collections.synchronizedMap(new HashMap<Long, C4DatabaseObserver>());

    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // hold pointer to C4DatabaseObserver
    private C4DatabaseObserverListener listener = null;
    private Object context = null;

    //-------------------------------------------------------------------------
    // Constructor
    //-------------------------------------------------------------------------
    public C4DatabaseObserver(Database db, C4DatabaseObserverListener listener, Object context) {
        this.listener = listener;
        this.context = context;
        handle = create(db._handle);
        reverseLookupTable.put(handle, this);
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public C4DatabaseChange[] getChanges(int maxChanges) {
        return getChanges(handle, maxChanges);
    }

    public void free() {
        if (handle != 0L) {
            reverseLookupTable.remove(handle);
            free(handle);
            handle = 0L;
        }
    }

    //-------------------------------------------------------------------------
    // protected methods
    //-------------------------------------------------------------------------
    @Override
    protected void finalize() throws Throwable {
        free();
        super.finalize();
    }

    //-------------------------------------------------------------------------
    // callback methods from JNI
    //-------------------------------------------------------------------------

    /**
     * Callback invoked by a database observer.
     * <p>
     * NOTE: Two parameters, observer and context, which are defined for iOS:
     * observer -> this instance
     * context ->  maintained in java layer
     */
    private static void callback(long handle) {
        C4DatabaseObserver obs = reverseLookupTable.get(handle);
        if (obs != null && obs.listener != null)
            obs.listener.callback(obs, obs.context);
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------

    private native static long create(long db);

    private native static C4DatabaseChange[] getChanges(long observer, int maxChanges);

    private static native void free(long c4observer);
}
