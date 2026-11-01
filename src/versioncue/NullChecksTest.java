public class NullChecksTest {
    int field;
    static int count;
    interface Call { long call(int i, long l, double d, Object o); }
    static class Holder { long value; Holder(long l) { value=l; } }
    class Inner { int value; Inner(int v) { value=v; } }
    static void expected(Runnable action) { try { action.run(); throw new Error("Missing NPE"); } catch(NullPointerException e) {count++;} }
    public static void main(String[] args) {
        expected(new Runnable(){public void run(){Object o=null; o.hashCode();}});
        expected(new Runnable(){public void run(){Call o=null; o.call(2,3L,4.5,new Object());}});
        expected(new Runnable(){public void run(){NullChecksTest o=null; int x=o.field;}});
        expected(new Runnable(){public void run(){Holder o=null; o.value=42L;}});
        expected(new Runnable(){public void run(){Object[] a=null; int x=a.length;}});
        expected(new Runnable(){public void run(){Object[] a=null; Object x=a[0];}});
        expected(new Runnable(){public void run(){long[] a=null; a[0]=2L;}});
        expected(new Runnable(){public void run(){double[] a=null; a[0]=2.5;}});
        expected(new Runnable(){public void run(){float[] a=null; a[0]=2.5f;}});
        expected(new Runnable(){public void run(){Object[] a=null; a[0]=new Object();}});
        expected(new Runnable(){public void run(){Object lock=null; synchronized(lock){}}});
        expected(new Runnable(){public void run(){throw (RuntimeException)null;}});
        NullChecksTest outer=new NullChecksTest();
        if(outer.new Inner(7).value!=7 || new Holder(14).value!=14)throw new Error("Constructor behavior");
        if(count!=12)throw new Error("Wrong count: "+count);
        System.out.println("PASS: 12 null operations, category-2 arguments/fields/arrays, inner constructors");
    }
}
