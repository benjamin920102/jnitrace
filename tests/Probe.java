package obf;
public class Probe {
    static native void install();
    native long instance(long x);
    static native boolean retBoolean(boolean x);
    static native byte retByte(byte x);
    static native char retChar(char x);
    static native short retShort(short x);
    static native int retInt(int x);
    static native float retFloat(float x);
    static native double retDouble(double x);

    static native void exercise();
    static native double mixed(boolean z, byte b, char c, short s, int i, long j, float f, double d, String text,
                               int k, double x, long y, float v, int w, double q);
    static native long child(long x);
    static native void thrower();
    static String echo(String s) { return s; }
    static long callback() { return child(1234567890123L); }
    public int field=7;
    public static String text="field";
    public Probe() {}
    static void check(boolean ok) { if(!ok)throw new AssertionError(); }
    public static void main(String[] args) throws Exception {
        System.load(java.nio.file.Paths.get(args[0]).toAbsolutePath().toString()); install();
        Runnable task=()->{for(int n=0;n<5;n++) {
            check(mixed(true,(byte)-7,'中',(short)-300,1234,9000000000L,1.25f,2.5,"A\u0000中😀",9,4.5,8000000000L,2.25f,-17,8.5)==101.25);
            check(retBoolean(true)==true);check(retByte((byte)-101)==(byte)-101);check(retChar('中')=='中');check(retShort((short)-30123)==(short)-30123);check(retInt(-123456789)==-123456789);check(retFloat(1.25f)==1.25f);check(retDouble(-3.5)==-3.5);
            check(new Probe().instance(9999999999L)==9999999998L);
            exercise();
            try {thrower();throw new AssertionError();}catch(IllegalStateException expected){}
        }};
        Thread a=new Thread(task),b=new Thread(task);a.setUncaughtExceptionHandler((t,e)->{e.printStackTrace();System.exit(2);});
        b.setUncaughtExceptionHandler((t,e)->{e.printStackTrace();System.exit(2);});a.start();b.start();a.join();b.join();
        System.out.println("PROBE_OK");
    }
}
