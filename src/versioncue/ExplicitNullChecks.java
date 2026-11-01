import java.lang.instrument.*;
import java.security.ProtectionDomain;
import org.objectweb.asm.*;
import org.objectweb.asm.commons.AdviceAdapter;

// Preserve Java's null semantics without relying on guest memory-fault delivery.
public final class ExplicitNullChecks implements ClassFileTransformer, Opcodes {
    private static boolean trace;
    public static void premain(String args, Instrumentation instrumentation) {
        trace = "trace".equals(args);
        instrumentation.addTransformer(new ExplicitNullChecks());
        System.err.println("[nullchecks] explicit null checks enabled");
    }
    public byte[] transform(ClassLoader loader, String name, Class<?> redefined, ProtectionDomain domain, byte[] bytes) {
        if (name == null || name.startsWith("org/objectweb/asm/") || name.equals("ExplicitNullChecks") || name.startsWith("ExplicitNullChecks$") || name.startsWith("java/lang/") || name.startsWith("java/security/") || name.startsWith("sun/")) return null;
        try {
            ClassReader reader = new ClassReader(bytes);
            final ClassWriter writer = new ClassWriter(ClassWriter.COMPUTE_MAXS);
            ClassAdapter adapter = new ClassAdapter(writer) {
                public MethodVisitor visitMethod(int access, String method, String desc, String signature, String[] exceptions) {
                    MethodVisitor visitor = super.visitMethod(access, method, desc, signature, exceptions);
                    if (visitor == null || (access & (ACC_ABSTRACT | ACC_NATIVE)) != 0) return visitor;
                    return new Checks(access, method, desc, visitor);
                }
            };
            reader.accept(adapter, 0);
            return writer.toByteArray();
        } catch (Throwable failure) {
            System.err.println("[nullchecks] cannot transform " + name + ": " + failure);
            return null;
        }
    }
    static class Checks extends AdviceAdapter {
        private boolean initialized;
        Checks(int access, String method, String descriptor, MethodVisitor visitor) { super(visitor, access, method, descriptor); }
        protected void onMethodEnter() { initialized = true; }
        private void check() {
            Label good = new Label();
            mv.visitInsn(DUP);
            mv.visitJumpInsn(IFNONNULL, good);
            mv.visitInsn(POP);
            mv.visitTypeInsn(NEW, "java/lang/NullPointerException");
            mv.visitInsn(DUP);
            mv.visitMethodInsn(INVOKESPECIAL, "java/lang/NullPointerException", "<init>", "()V");
            if (trace) {
                mv.visitInsn(DUP);
                mv.visitMethodInsn(INVOKEVIRTUAL, "java/lang/Throwable", "printStackTrace", "()V");
            }
            mv.visitInsn(ATHROW);
            mv.visitLabel(good);
        }
        private int save(Type type) {
            int local = newLocal(type);
            mv.visitVarInsn(type.getOpcode(ISTORE), local);
            return local;
        }
        private void load(Type type, int local) { mv.visitVarInsn(type.getOpcode(ILOAD), local); }
        public void visitMethodInsn(int opcode, String owner, String name, String desc) {
            if (opcode == INVOKESTATIC || name.equals("<init>")) { super.visitMethodInsn(opcode, owner, name, desc); return; }
            Type[] arguments = Type.getArgumentTypes(desc);
            int[] locals = new int[arguments.length];
            for (int i=arguments.length-1; i>=0; i--) locals[i] = save(arguments[i]);
            check();
            for (int i=0; i<arguments.length; i++) load(arguments[i], locals[i]);
            super.visitMethodInsn(opcode, owner, name, desc);
        }
        public void visitFieldInsn(int opcode, String owner, String name, String desc) {
            if (opcode == GETFIELD && initialized) check();
            if (opcode == PUTFIELD && initialized) { Type type = Type.getType(desc); int local = save(type); check(); load(type, local); }
            super.visitFieldInsn(opcode, owner, name, desc);
        }
        public void visitInsn(int opcode) {
            if (opcode == ARRAYLENGTH || opcode == MONITORENTER || opcode == MONITOREXIT || opcode == ATHROW) check();
            if (opcode >= IALOAD && opcode <= SALOAD) {
                int index = save(Type.INT_TYPE); check(); load(Type.INT_TYPE, index);
            }
            if (opcode >= IASTORE && opcode <= SASTORE) {
                Type type = opcode == LASTORE ? Type.LONG_TYPE : opcode == FASTORE ? Type.FLOAT_TYPE : opcode == DASTORE ? Type.DOUBLE_TYPE : opcode == AASTORE ? Type.getType(Object.class) : Type.INT_TYPE;
                int value = save(type); int index = save(Type.INT_TYPE); check(); load(Type.INT_TYPE, index); load(type, value);
            }
            super.visitInsn(opcode);
        }
    }
}
