# blender_gui.py: Blender's window and 3-D viewport, on the scene blender_func.py saved.
#   blender --factory-startup -noaudio OUTDIR/scene.blend -P blender_gui.py -- OUTDIR
# (a .blend on the command line also keeps the splash screen away). Once the window's event loop
# runs, shows the scene through the camera in the 3-D view (OpenGL, solid shading), takes a
# screenshot of the window with Blender's own operator, checks that the red, green, blue and
# yellow objects are on screen, and quits Blender. Prints "RESULT ..." lines.
import bpy, os, sys
argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
out = argv[0] if argv else "/tmp/blender-gui"
os.makedirs(out, exist_ok=True)
import traceback

def gres(name, ok, info=""):
    print("RESULT %s %s %s" % (name, "PASS" if ok else "FAIL", info)); sys.stdout.flush()

def viewport_check(context):
    win = context.window
    areas = [a for a in win.screen.areas if a.type == 'VIEW_3D']
    gres("gui_view3d", len(areas) > 0, "%d 3-D views" % len(areas))
    if not areas: return
    area = areas[0]; sp = area.spaces[0]
    sp.viewport_shade = 'SOLID'; sp.region_3d.view_perspective = 'CAMERA'
    region = [r for r in area.regions if r.type == 'WINDOW'][0]
    ov = {"window": win, "screen": win.screen, "area": area, "region": region, "scene": context.scene}
    bpy.ops.wm.redraw_timer(ov, type='DRAW_WIN_SWAP', iterations=2)
    shot = os.path.join(out, "viewport.png")
    bpy.ops.screen.screenshot(ov, filepath=shot, full=True)
    ok = os.path.exists(shot) and os.path.getsize(shot) > 5000
    gres("gui_screenshot", ok, shot)
    if not ok: return
    im = bpy.data.images.load(shot); w, h = im.size; px = list(im.pixels); im.user_clear(); bpy.data.images.remove(im)
    # count strongly red, green, blue and yellow pixels (the interface's icons have few of them)
    x0, y0, rw, rh = 0, 0, w, h
    cnt = {"red": 0, "green": 0, "blue": 0, "yellow": 0}
    for y in range(max(0, y0), min(h, y0 + rh), 2):
        for x in range(max(0, x0), min(w, x0 + rw), 2):
            i = (y * w + x) * 4; r, g, b = px[i], px[i + 1], px[i + 2]
            if r > 0.35 and r > 2.2 * g and r > 2.2 * b: cnt["red"] += 1
            elif g > 0.3 and g > 1.8 * r and g > 1.8 * b: cnt["green"] += 1
            elif b > 0.35 and b > 1.8 * r and b > 1.6 * g: cnt["blue"] += 1
            elif r > 0.35 and g > 0.3 and b < 0.45 * min(r, g): cnt["yellow"] += 1
    gres("gui_viewport_objects", all(v > 300 for v in cnt.values()), " ".join("%s %d" % kv for kv in sorted(cnt.items())))

class ROSETTUS_OT_viewport_test(bpy.types.Operator):
    """the viewport check, run from the window's event loop once the window is up"""
    bl_idname = "wm.rosettus_viewport_test"; bl_label = "Rosettus viewport test"
    ticks = 0
    def modal(self, context, event):
        if event.type == 'TIMER':
            self.ticks += 1
            if self.ticks == 3:
                context.window_manager.event_timer_remove(self.timer)
                try: viewport_check(context)
                except Exception:
                    traceback.print_exc(file=sys.stdout); gres("gui_script", False, "exception")
                print("RESULT blender_gui done"); sys.stdout.flush()
                bpy.ops.wm.quit_blender()
                return {'FINISHED'}
        return {'PASS_THROUGH'}
    def invoke(self, context, event):
        self.timer = context.window_manager.event_timer_add(0.5, context.window)
        context.window_manager.modal_handler_add(self)
        return {'RUNNING_MODAL'}
bpy.utils.register_class(ROSETTUS_OT_viewport_test)

def start(scene):
    if bpy.context.window:
        bpy.app.handlers.scene_update_post.remove(start)
        bpy.ops.wm.rosettus_viewport_test('INVOKE_DEFAULT')
bpy.app.handlers.scene_update_post.append(start)
