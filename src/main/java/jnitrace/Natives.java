package jnitrace;

final class Natives {
    private Natives() {}
    static native boolean init(String jsonlPath, String moduleFilter, String dumpDir, boolean logUnknown,
                              String classPatterns, int depth, boolean compact, boolean flushEach);
    static native void shutdown();
}
