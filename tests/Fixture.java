package obf;
public class Fixture {
    static native void run();
    static int value;
    static String echo(String s) { return s; }
    static void fail() { throw new IllegalStateException("expected"); }
    public static void main(String[] args) throws Exception {
        System.load(java.nio.file.Paths.get(args[0]).toAbsolutePath().toString());
        Thread a = new Thread(() -> { for (int i=0;i<200;i++) run(); });
        Thread b = new Thread(() -> { for (int i=0;i<200;i++) run(); });
        a.start(); b.start(); a.join(); b.join();
    }
}
