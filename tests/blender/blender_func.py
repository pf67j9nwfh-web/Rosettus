# blender_func.py: a functional test of Blender 2.6x itself (not its splash screen or website link).
# Run in the background, with no window and no sound:
#   blender -b --factory-startup -noaudio -P blender_func.py -- OUTDIR
# Builds a scene (meshes, modifiers, edit-mode operators, materials, a keyframed animation, a
# particle system), renders it with Blender Internal to PNG, checks the pixels, saves the .blend,
# opens it again, checks that the scene came back the same and renders it again. Prints
# "RESULT <check> PASS|FAIL ..." lines and a "VALUES" line of numbers to compare between
# machines (a real PowerPC Mac or 10.6 Rosetta against Rosettus).
import bpy, sys, os, math, hashlib

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
out = argv[0] if argv else "/tmp/blender-func"
os.makedirs(out, exist_ok=True)
fails = 0
values = []

def res(name, ok, info=""):
    global fails
    if not ok: fails += 1
    print("RESULT %s %s %s" % (name, "PASS" if ok else "FAIL", info))
    sys.stdout.flush()

def near(a, b, eps=1e-4): return abs(a - b) <= eps * max(1.0, abs(a), abs(b))

scene = bpy.context.scene

# 1. start from an empty scene
for o in list(scene.objects):
    scene.objects.unlink(o)
res("empty_scene", len(scene.objects) == 0, "%d objects" % len(scene.objects))

# 2. primitives and their exact sizes
bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, size=1.0, location=(0, 0, 1))
sphere = bpy.context.active_object; sphere.name = "Ball"
res("uv_sphere", len(sphere.data.vertices) == 32 * 15 + 2 and len(sphere.data.polygons) == 32 * 16,
    "%d verts %d faces" % (len(sphere.data.vertices), len(sphere.data.polygons)))
bpy.ops.mesh.primitive_torus_add(major_radius=1.5, minor_radius=0.3, major_segments=48, minor_segments=12, location=(0, 0, 1))
torus = bpy.context.active_object; torus.name = "Ring"
res("torus", len(torus.data.vertices) == 48 * 12, "%d verts" % len(torus.data.vertices))
bpy.ops.mesh.primitive_plane_add(location=(0, 0, 0))
plane = bpy.context.active_object; plane.name = "Floor"; plane.scale = (6, 6, 1)
bpy.ops.mesh.primitive_cube_add(location=(3, 0, 0.5))
cube = bpy.context.active_object; cube.name = "Box"; cube.scale = (0.5, 0.5, 0.5)

# 3. a subdivision surface modifier, applied: the vertex count of Catmull-Clark level 2 on a cube
mod = cube.modifiers.new("Subsurf", 'SUBSURF'); mod.levels = 2
bpy.context.scene.objects.active = cube
bpy.ops.object.modifier_apply(apply_as='DATA', modifier="Subsurf")
res("subsurf_apply", len(cube.data.vertices) == 98, "%d verts (98 expected)" % len(cube.data.vertices))
# the subdivided cube is rounder: every vertex within the original cube, the corners pulled in
rmax = max(v.co.length for v in cube.data.vertices)
res("subsurf_shape", 0.8 < rmax < math.sqrt(3) - 0.05, "max radius %.5f" % rmax)
values.append(round(rmax, 5))

# 4. edit mode: extrude the top faces of a second cube and move them up
bpy.ops.mesh.primitive_cube_add(location=(-3, 0, 0.5))
tower = bpy.context.active_object; tower.name = "Tower"; tower.scale = (0.5, 0.5, 0.5)
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='DESELECT')
bpy.ops.object.mode_set(mode='OBJECT')
for p in tower.data.polygons:
    p.select = p.normal.z > 0.9
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.extrude_region_move(TRANSFORM_OT_translate={"value": (0, 0, 2.0)})
bpy.ops.object.mode_set(mode='OBJECT')
zmax = max(v.co.z for v in tower.data.vertices)
res("extrude", len(tower.data.vertices) == 12 and near(zmax, 1.0 + 2.0 / 0.5), "%d verts top z %.4f" % (len(tower.data.vertices), zmax))

# 5. volume of the torus mesh from its triangles (divergence theorem): 2 pi^2 R r^2 for the smooth one
def volume(ob):
    me = ob.data; v = me.vertices; vol = 0.0
    for p in me.polygons:
        idx = list(p.vertices)
        for k in range(1, len(idx) - 1):
            a, b, c = v[idx[0]].co, v[idx[k]].co, v[idx[k + 1]].co
            vol += a.dot(b.cross(c)) / 6.0
    return vol
tv = volume(torus); ideal = 2 * math.pi ** 2 * 1.5 * 0.3 ** 2
res("torus_volume", 0.95 * ideal < tv < ideal, "%.6f (smooth %.6f)" % (tv, ideal))
values.append(round(tv, 6))

# 6. materials
def mat(name, rgb):
    m = bpy.data.materials.new(name); m.diffuse_color = rgb; m.specular_intensity = 0.1; return m
sphere.data.materials.append(mat("Red", (0.9, 0.05, 0.05)))
torus.data.materials.append(mat("Green", (0.05, 0.8, 0.1)))
plane.data.materials.append(mat("Grey", (0.6, 0.6, 0.6)))
cube.data.materials.append(mat("Blue", (0.05, 0.1, 0.9)))
tower.data.materials.append(mat("Yellow", (0.9, 0.8, 0.05)))

# 7. a keyframed animation, evaluated between the keys (Bezier, so not linear)
torus.location = (0, 0, 1); torus.keyframe_insert("location", frame=1)
torus.location = (0, 0, 3); torus.keyframe_insert("location", frame=21)
torus.rotation_euler = (0, 0, 0); torus.keyframe_insert("rotation_euler", frame=1)
torus.rotation_euler = (math.radians(90), 0, 0); torus.keyframe_insert("rotation_euler", frame=21)
fc = [f for f in torus.animation_data.action.fcurves if f.data_path == "location" and f.array_index == 2][0]
z11, z6 = fc.evaluate(11), fc.evaluate(6)
res("fcurve", near(z11, 2.0, 1e-3) and 1.0 < z6 < 1.5, "z(11)=%.5f z(6)=%.5f" % (z11, z6))
values += [round(z6, 5)]
scene.frame_set(6)
res("frame_set", near(torus.location.z, z6, 1e-4), "z %.5f" % torus.location.z)
scene.frame_set(1)

# 8. a particle system on the floor, simulated for a few frames (deterministic: fixed seed)
plane.modifiers.new("Parts", 'PARTICLE_SYSTEM')
ps = plane.particle_systems[0]; st = ps.settings
st.count = 200; st.frame_start = 1; st.frame_end = 1; st.lifetime = 100
st.normal_factor = 2.0; st.effector_weights.gravity = 1.0; ps.seed = 7
for f in range(1, 11): scene.frame_set(f)
zs = [p.location.z for p in ps.particles]
alive = sum(1 for p in ps.particles if p.alive_state == 'ALIVE')
# launched at 2 units/s up at frame 1, under gravity (9.81), 9 frames later: close to z = 2t - g t^2 / 2
tt = 9.0 / scene.render.fps; zt = 2.0 * tt - 9.81 * tt * tt / 2
res("particles", len(zs) == 200 and alive == 200 and abs(max(zs) - zt) < 0.03, "%d particles, %d alive, max z %.4f (ballistic %.4f)" % (len(zs), alive, max(zs) if zs else 0, zt))
values.append(round(sum(zs) / max(1, len(zs)), 5))
scene.frame_set(1)
plane.modifiers.remove(plane.modifiers["Parts"])

# 9. camera, light, render settings; render to PNG
bpy.ops.object.camera_add(location=(0, -9, 4.5))
cam = bpy.context.active_object; cam.rotation_euler = (math.radians(66), 0, 0); scene.camera = cam
bpy.ops.object.lamp_add(type='SUN', location=(2, -3, 8))
sun = bpy.context.active_object; sun.rotation_euler = (math.radians(30), math.radians(20), 0)
sun.data.energy = 1.2; sun.data.shadow_method = 'RAY_SHADOW'
r = scene.render
r.engine = 'BLENDER_RENDER'; r.resolution_x = 320; r.resolution_y = 240; r.resolution_percentage = 100
r.use_antialiasing = True; r.antialiasing_samples = '8'; r.use_raytrace = True; r.threads_mode = 'AUTO'
r.image_settings.file_format = 'PNG'; r.image_settings.color_mode = 'RGB'
scene.world.horizon_color = (0.2, 0.25, 0.35)

def render(path):
    r.filepath = path
    bpy.ops.render.render(write_still=True)
    return os.path.exists(path) and os.path.getsize(path) > 1000

def pixels(path):
    im = bpy.data.images.load(path); w, h = im.size; px = list(im.pixels); im.user_clear(); bpy.data.images.remove(im)
    return w, h, px

def at(px, w, x, y):   # y from the bottom
    i = (y * w + x) * 4; return px[i:i + 3]

p1 = os.path.join(out, "render1.png")
ok = render(p1)
res("render", ok, p1)
if ok:
    w, h, px = pixels(p1)
    res("render_size", (w, h) == (320, 240), "%dx%d" % (w, h))
    # where the objects project: use the camera to find each object's centre on screen
    def screen(ob, z=0):   # perspective projection by the camera (horizontal field of view: w > h)
        import mathutils
        p = cam.matrix_world.inverted() * (ob.matrix_world.to_translation() + mathutils.Vector((0, 0, z)))
        k = 1.0 / (2.0 * math.tan(cam.data.angle / 2.0))
        return int((0.5 + k * p.x / -p.z) * w), int((0.5 + k * (w / h) * p.y / -p.z) * h)
    checks = [("Ball", sphere, 0, 0), ("Box", cube, 2, 0), ("Tower", tower, 0, 0.8)]
    for name, ob, ch, dz in checks:
        x, y = screen(ob, dz)
        c = at(px, w, x, y)
        dom = c[ch] > 1.5 * max(c[(ch + 1) % 3], c[(ch + 2) % 3]) if ch != 0 or name == "Ball" else False
        if name == "Tower": dom = c[0] > 0.3 and c[1] > 0.25 and c[2] < 0.5 * min(c[0], c[1])
        res("pixel_" + name, dom, "at %d,%d rgb %.3f %.3f %.3f" % (x, y, c[0], c[1], c[2]))
    # ray-traced shadows on the floor: some pixels nearly black
    dark = sum(1 for i in range(0, len(px), 4) if px[i] < 0.08 and px[i + 1] < 0.08 and px[i + 2] < 0.08)
    res("shadows", 0.01 * w * h < dark < 0.2 * w * h, "%d dark pixels" % dark)
    mean = sum(px[i] for i in range(0, len(px), 4)) / (w * h)
    values.append(round(mean, 4))
    res("image_mean", 0.05 < mean < 0.9, "mean red %.4f" % mean)

# 10. save, open again, compare, render again
bf = os.path.join(out, "scene.blend")
bpy.ops.wm.save_as_mainfile(filepath=bf)
res("save", os.path.exists(bf) and os.path.getsize(bf) > 50000, "%d bytes" % (os.path.getsize(bf) if os.path.exists(bf) else 0))
before = {o.name: (len(o.data.vertices) if o.type == 'MESH' else -1, tuple(round(x, 4) for x in o.location)) for o in scene.objects}
bpy.ops.wm.open_mainfile(filepath=bf)
scene = bpy.context.scene; r = scene.render
after = {o.name: (len(o.data.vertices) if o.type == 'MESH' else -1, tuple(round(x, 4) for x in o.location)) for o in scene.objects}
res("reopen", before == after, "%d objects" % len(after))
mats = sorted(set(m.name for o in scene.objects if o.type == 'MESH' for m in o.data.materials if m))
res("reopen_materials", mats == ["Blue", "Green", "Grey", "Red", "Yellow"], ",".join(mats))
t = bpy.data.objects["Ring"]; scene.frame_set(6)
res("reopen_animation", near(t.location.z, values[2], 1e-4), "z %.5f" % t.location.z)
scene.frame_set(1)
p2 = os.path.join(out, "render2.png")
if render(p2) and ok:
    w2, h2, px2 = pixels(p2)
    diff = max(abs(a - b) for a, b in zip(px, px2)) if len(px) == len(px2) else 1.0
    res("rerender_same", diff < 0.02, "max channel difference %.4f" % diff)
print("VALUES " + " ".join(str(v) for v in values))
print("RESULT blender_func %s (%d failed)" % ("PASS" if fails == 0 else "FAIL", fails))
sys.stdout.flush()
