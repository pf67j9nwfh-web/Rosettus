# mksamples.py (Mojave, system python 2.7): sample documents for Rosettus testing in ~/Documents/RosettusSamples
import os, math, subprocess
import Quartz as Q
from Foundation import NSURL
D = os.path.expanduser('~/Documents/RosettusSamples')
if not os.path.isdir(D): os.makedirs(D)
W, H = 800, 600
cs = Q.CGColorSpaceCreateDeviceRGB()
ctx = Q.CGBitmapContextCreate(None, W, H, 8, 0, cs, Q.kCGImageAlphaPremultipliedLast)
for x in range(0, W, 4):
    for y in range(0, H, 20):
        Q.CGContextSetRGBFillColor(ctx, x / float(W), y / float(H), 0.5 + 0.5 * math.sin(x / 60.0), 1)
        Q.CGContextFillRect(ctx, ((x, y), (4, 20)))
Q.CGContextSetRGBStrokeColor(ctx, 1, 1, 1, 1); Q.CGContextSetLineWidth(ctx, 6)
Q.CGContextStrokeEllipseInRect(ctx, ((250, 150), (300, 300)))
Q.CGContextSelectFont(ctx, "Helvetica-Bold", 48, Q.kCGEncodingMacRoman)
Q.CGContextSetRGBFillColor(ctx, 1, 1, 1, 1)
Q.CGContextShowTextAtPoint(ctx, 200, 520, "Rosettus sample", 15)
img = Q.CGBitmapContextCreateImage(ctx)
def save(name, uti, props=None):
    d = Q.CGImageDestinationCreateWithURL(NSURL.fileURLWithPath_(os.path.join(D, name)), uti, 1, None)
    Q.CGImageDestinationAddImage(d, img, props); Q.CGImageDestinationFinalize(d)
save('RosettusSample.jpg', 'public.jpeg', {Q.kCGImageDestinationLossyCompressionQuality: 0.9})
save('RosettusSample.png', 'public.png')
save('RosettusSample.tif', 'public.tiff')
save('RosettusSample.gif', 'com.compuserve.gif')
save('RosettusSample.bmp', 'com.microsoft.bmp')
paras = ''.join('<p>%sParagraph %d: The quick brown fox jumps over the lazy dog.%s</p>\n' % ('<b>' if i % 10 == 0 else '', i, '</b>' if i % 10 == 0 else '') for i in range(1, 61))
html = '''<html><head><title>Rosettus sample</title></head><body>
<h1>Rosettus sample document</h1>
%s
<table border="1" cellpadding="4"><tr><th>A</th><th>B</th><th>C</th></tr><tr><td>1</td><td>2</td><td>3</td></tr><tr><td>4</td><td>5</td><td>6</td></tr></table>
<p><img src="RosettusSample.jpg" width="400" height="300" alt="sample"></p>
</body></html>
''' % paras
open(os.path.join(D, 'RosettusSample.html'), 'w').write(html)
for fmt in ('doc', 'rtf', 'docx'):
    subprocess.call(['textutil', '-convert', fmt, os.path.join(D, 'RosettusSample.html'), '-output', os.path.join(D, 'RosettusSample.' + fmt)])
rows = [('Month', 'Sales', 'Cost')] + [(m, 800 + i * 97 % 1200, 300 + i * 53 % 400) for i, m in enumerate('Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec'.split())]
open(os.path.join(D, 'RosettusSample.csv'), 'w').write('\r'.join(','.join(str(c) for c in r) for r in rows) + '\r')
x = ['<?xml version="1.0"?>', '<?mso-application progid="Excel.Sheet"?>',
     '<Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet" xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet">',
     '<Worksheet ss:Name="Data"><Table>']
for i, r in enumerate(rows):
    cells = ''.join('<Cell><Data ss:Type="%s">%s</Data></Cell>' % ('String' if (i == 0 or j == 0) else 'Number', c) for j, c in enumerate(r))
    if i == 0: cells += '<Cell><Data ss:Type="String">Profit</Data></Cell>'
    else: cells += '<Cell ss:Formula="=RC[-2]-RC[-1]"><Data ss:Type="Number">0</Data></Cell>'
    x.append('<Row>%s</Row>' % cells)
x.append('<Row><Cell><Data ss:Type="String">Total</Data></Cell><Cell ss:Formula="=SUM(R2C:R13C)"><Data ss:Type="Number">0</Data></Cell><Cell ss:Formula="=SUM(R2C:R13C)"><Data ss:Type="Number">0</Data></Cell><Cell ss:Formula="=SUM(R2C:R13C)"><Data ss:Type="Number">0</Data></Cell></Row>')
x.append('</Table></Worksheet></Workbook>')
open(os.path.join(D, 'RosettusSample.xml'), 'w').write('\n'.join(x))
for f in sorted(os.listdir(D)): print f, os.path.getsize(os.path.join(D, f))
