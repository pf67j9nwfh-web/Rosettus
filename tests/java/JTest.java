import java.util.*;

public class JTest {
    public static void main(String[] a) throws Exception {
        long t0 = System.currentTimeMillis();
        final int[] sums = new int[4];
        Thread[] th = new Thread[4];
        for (int i = 0; i < 4; i++) {
            final int k = i;
            th[i] = new Thread() { public void run() {
                int s = 0; for (int j = 0; j < 2000000; j++) s += (j ^ k) % 7; sums[k] = s; } };
            th[i].start();
        }
        for (int i = 0; i < 4; i++) th[i].join();
        Map m = new HashMap();
        for (int i = 0; i < 200000; i++) m.put("k" + i, new Integer(i));
        StringBuffer sb = new StringBuffer();
        for (int i = 0; i < 1000; i++) sb.append(i % 10);
        List l = new ArrayList();
        for (int r = 0; r < 20; r++) { l.clear(); for (int i = 0; i < 100000; i++) l.add(new byte[64]); }
        System.gc();
        Collections.sort(new ArrayList(m.keySet()));
        System.out.println("JTest ok " + System.getProperty("java.version") + " threads=" + sums[0] + "," + sums[3]
            + " map=" + m.size() + " sb=" + sb.length() + " ms=" + (System.currentTimeMillis() - t0));
    }
}
