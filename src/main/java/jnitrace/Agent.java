package jnitrace;

import java.io.*;
import java.lang.instrument.Instrumentation;
import java.nio.file.*;
import java.util.*;

/**
 * Java agent that loads a bundled native JNI-call tracer.
 *
 * Usage: java -javaagent:jnitrace.jar[=opt,opt,...] -jar target.jar
 * Options (comma separated):
 *   module=<substr>   only log calls made from native modules whose file name contains substr
 *                     (e.g. module=native0  /  module=target.dll). Strongly recommended.
 *   out=<dir>         output directory (default: Windows Desktop)
 *   dump=1            also dump classes passed to DefineClass into <out>/defined_classes
 *   include=<globs>     only log JNI calls made while a native method of a matching Java class is running.
 *                     Separate patterns with | (e.g. class=native.*|obf.*). Dotted class names, * wildcard.
 *   depth=<n>         Java frames (from top) tested against class= (default 1 = the running native method;
 *                     use 4-8 to also catch JNI_OnLoad triggered from a matching class)
 *   compact=0       inline metadata instead of the default compact contexts
 *   flush=1         flush each event instead of using a 64 KiB buffer
 *   unknown=1         also log calls made from unknown (anonymous) memory when module= is set
 */
public final class Agent {
    private Agent() {}

    public static void premain(String args, Instrumentation inst) { start(args); }
    public static void agentmain(String args, Instrumentation inst) { start(args); }

    private static void start(String args) {
        Map<String, String> o = new HashMap<>();
        if (args != null) for (String kv : args.split(",")) {
            int i = kv.indexOf('=');
            if (i > 0) o.put(kv.substring(0, i).trim().toLowerCase(Locale.ROOT), kv.substring(i + 1).trim());
        }
        try {
            Path dir = o.containsKey("out") ? Paths.get(o.get("out")) : desktop();
            Files.createDirectories(dir);
            Path jsonl = dir.resolve("jni_trace.jsonl");
            String dump = null;
            if ("1".equals(o.get("dump"))) {
                Path dd = dir.resolve("defined_classes");
                Files.createDirectories(dd);
                dump = dd.toString();
            }
            System.load(extractLib().toAbsolutePath().toString());
            boolean ok = Natives.init(jsonl.toString(), o.get("module"), dump, "1".equals(o.get("unknown")),
                    o.getOrDefault("include", o.get("class")), o.containsKey("depth") ? Integer.parseInt(o.get("depth")) : 1,
                    !"0".equals(o.get("compact")), "1".equals(o.get("flush")));
            if (!ok) { System.err.println("[jnitrace] native init failed"); return; }
            Runtime.getRuntime().addShutdownHook(new Thread(Natives::shutdown, "jnitrace-flush"));
            System.err.println("[jnitrace] tracing -> " + jsonl);
        } catch (Throwable t) {
            System.err.println("[jnitrace] failed to start:");
            t.printStackTrace();
        }
    }

    private static Path desktop() {
        String home = System.getProperty("user.home");
        List<Path> c = new ArrayList<>();
        String od = System.getenv("OneDrive");
        if (od != null) c.add(Paths.get(od, "Desktop"));
        String up = System.getenv("USERPROFILE");
        if (up != null) c.add(Paths.get(up, "Desktop"));
        c.add(Paths.get(home, "Desktop"));
        for (Path p : c) if (Files.isDirectory(p)) return p;
        return Paths.get(home);
    }

    private static Path extractLib() throws IOException {
        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        String arch = System.getProperty("os.arch", "");
        if (!(arch.equals("amd64") || arch.equals("x86_64")))
            throw new IOException("This build supports x64 JVMs only: " + arch);
        if (!os.contains("win") && !os.contains("linux"))
            throw new IOException("Unsupported operating system: " + os);
        String res = os.contains("win") ? "jnitrace.dll" : "libjnitrace.so";
        try (InputStream in = Agent.class.getResourceAsStream("/native/" + res)) {
            if (in == null) throw new FileNotFoundException("missing resource /native/" + res);
            Path tmp = Files.createTempFile("jnitrace_", os.contains("win") ? ".dll" : ".so");
            tmp.toFile().deleteOnExit();
            Files.copy(in, tmp, StandardCopyOption.REPLACE_EXISTING);
            return tmp;
        }
    }

}
