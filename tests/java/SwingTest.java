import javax.swing.*;
import java.awt.*;
import java.awt.event.*;

public class SwingTest {
    public static void main(String[] a) throws Exception {
        final JFrame f = new JFrame("Rosettus Swing " + System.getProperty("java.version"));
        final JLabel lab = new JLabel("not clicked");
        final JButton b = new JButton("Click me");
        final JTextField tf = new JTextField("text field", 20);
        b.addActionListener(new ActionListener() { public void actionPerformed(ActionEvent e) { lab.setText("clicked"); } });
        JPanel p = new JPanel(new FlowLayout());
        p.add(b); p.add(tf); p.add(lab);
        f.getContentPane().add(p);
        f.setSize(420, 140);
        f.setLocation(200, 200);
        f.setVisible(true);
        Thread.sleep(2500);
        SwingUtilities.invokeAndWait(new Runnable() { public void run() { b.doClick(); } });
        Thread.sleep(500);
        System.out.println("SwingTest " + lab.getText() + " laf=" + UIManager.getLookAndFeel().getName()
            + " visible=" + f.isShowing() + " size=" + f.getSize().width + "x" + f.getSize().height);
        Thread.sleep(2000);
        System.exit(0);
    }
}
