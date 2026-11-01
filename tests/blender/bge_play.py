# tests/blender/bge_play.py -- the Blender Game Engine through blenderplayer, under Rosettus.
#   blender -b --factory-startup -P bge_play.py -- make  OUTDIR [GLSL|MULTITEXTURE]
#   blender -b --factory-startup -P bge_play.py -- check OUTDIR
# "make" saves OUTDIR/game.blend: a red plane, a blue rigid-body ball dropped from z=10, a sun
# lamp and a camera whose Python controller logs the ball's height, takes a screenshot at
# tic 150 and ends the game at tic 240 (writing OUTDIR/bge.txt).  "check" reads bge.txt and
# the screenshot and prints RESULT lines: gravity and collision, real-time logic rate, lit
# colours (lighting needs the normal array), letterbox framing.
import bpy, sys, os
args = sys.argv[sys.argv.index('--') + 1:]
mode, out = args[0], args[1]

def result(name, ok, info=''):
    print('RESULT %s %s %s' % (name, 'PASS' if ok else 'FAIL', info)); sys.stdout.flush()

if mode == 'make':
    sc = bpy.context.scene
    for o in list(sc.objects):
        sc.objects.unlink(o)
    sc.render.engine = 'BLENDER_GAME'
    sc.game_settings.material_mode = args[2] if len(args) > 2 else 'GLSL'
    sc.world.horizon_color = (0.0, 0.6, 0.0)

    def mat(name, rgb):
        m = bpy.data.materials.new(name)
        m.diffuse_color = rgb; m.diffuse_intensity = 1.0; m.specular_intensity = 0.0
        return m
    bpy.ops.mesh.primitive_plane_add(location=(0, 0, 0))
    g = bpy.context.object; g.name = 'Ground'; g.scale = (6, 6, 1)
    g.data.materials.append(mat('Red', (0.9, 0.05, 0.05))); g.game.physics_type = 'STATIC'
    bpy.ops.mesh.primitive_uv_sphere_add(location=(0, 0, 10), size=1.0)
    b = bpy.context.object; b.name = 'Ball'
    b.data.materials.append(mat('Blue', (0.05, 0.05, 0.9)))
    b.game.physics_type = 'RIGID_BODY'; b.game.use_collision_bounds = True
    b.game.collision_bounds_type = 'SPHERE'; b.game.collision_margin = 0.0
    bpy.ops.object.lamp_add(type='SUN', location=(4, -4, 10))
    bpy.ops.object.camera_add(location=(0, -14, 5), rotation=(1.3, 0, 0))
    cam = bpy.context.object; cam.name = 'Cam'; sc.camera = cam
    txt = bpy.data.texts.new('probe.py')
    txt.write('''import bge, time
g = bge.logic
own = g.getCurrentController().owner
n = own.get('n', 0) + 1; own['n'] = n
b = g.getCurrentScene().objects['Ball']
OUT = %r
if n == 1:
    g.t0 = time.time(); g.lines = []          # not a game property: those are 32-bit floats
if n in (1, 20, 40, 60, 90, 120, 180):
    g.lines.append('Z %%d %%.3f' %% (n, b.worldPosition.z))
if n == 150:
    bge.render.makeScreenshot(OUT + '/shot.png')
if n == 240:
    f = open(OUT + '/bge.txt', 'w')
    f.write('\\n'.join(g.lines) + '\\nSECONDS %%.2f\\nWINDOW %%d %%d\\n' %% (time.time() - g.t0,
            bge.render.getWindowWidth(), bge.render.getWindowHeight()))
    f.close()
    g.endGame()
''' % out)
    sc.objects.active = cam
    bpy.ops.logic.sensor_add(type='ALWAYS', object='Cam')
    bpy.ops.logic.controller_add(type='PYTHON', object='Cam')
    s = cam.game.sensors[-1]; c = cam.game.controllers[-1]
    s.use_pulse_true_level = True; c.mode = 'SCRIPT'; c.text = txt; s.link(c)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, 'game.blend'))
    print('MADE', os.path.join(out, 'game.blend'))
else:
    v = {}
    try:
        for l in open(os.path.join(out, 'bge.txt')).read().split('\n'):
            t = l.split()
            if len(t) >= 3 and t[0] == 'Z': v['Z' + t[1]] = [t[2]]
            elif t: v[t[0]] = t[1:]
    except IOError:
        result('bge_ran', False, 'no bge.txt'); sys.exit(0)
    result('bge_ran', True)
    z60, z180 = float(v['Z60'][0]), float(v['Z180'][0])
    result('bge_gravity', 4.9 < z60 < 5.6, 'z(60 tics)=%.3f' % z60)        # 10 - g/2 at 1 s, Bullet steps
    result('bge_collision', abs(z180 - 1.0) < 0.02, 'z(180 tics)=%.3f' % z180)
    sec = float(v['SECONDS'][0])
    result('bge_realtime', 3.6 < sec < 5.0, '%.2f s for 239 tics at 60 Hz' % sec)
    result('bge_window', v['WINDOW'] == ['640', '480'], ' '.join(v['WINDOW']))
    import glob
    shots = glob.glob(os.path.join(out, 'shot*'))
    shot = shots[0] if shots else os.path.join(out, 'shot.png')
    if not os.path.exists(shot):
        result('bge_screenshot', False, 'missing'); sys.exit(0)
    im = bpy.data.images.load(shot); w, h = im.size; px = list(im.pixels)
    def at(x, ytop):                       # pixels are stored bottom row first
        i = ((h - 1 - ytop) * w + x) * 4
        return px[i:i + 3]
    def show(p): return '(%.2f %.2f %.2f)' % tuple(p)
    r = at(320, 380); bl = at(320, 210); sky = at(320, 100); box = at(320, 10)
    result('bge_lit_plane', r[0] > 0.6 and r[1] < 0.35 and r[2] < 0.35, 'plane ' + show(r))
    result('bge_lit_ball', bl[2] > 0.5 and bl[0] < 0.35 and bl[1] < 0.35, 'ball top ' + show(bl))
    lo = at(320, 280)                      # the ball's underside faces away from the sun
    result('bge_normals', bl[2] - lo[2] > 0.3, 'ball top %s vs underside %s' % (show(bl), show(lo)))
    result('bge_world', sky[1] > 0.6 and sky[0] < 0.2 and sky[2] < 0.2, 'sky ' + show(sky))
    result('bge_letterbox', max(box) < 0.05, '16:9 scene in 4:3 window ' + show(box))
