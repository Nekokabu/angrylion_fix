import sys,re
last_scene=None; t0=0; dp0=0; fr=0; dp=0
for l in open(sys.argv[1]):
    f=l.split()
    if len(f)<4 or not re.match(r'^\d+:',f[0]): continue
    fr=int(f[0][:-1]); scene=int(f[1].split('=')[1],16); dp=int(f[3].split('=')[1])
    if scene!=last_scene:
        if last_scene is not None:
            print(f"scene {last_scene:2d} VI {t0:5d}-{fr:5d} ({t0/60:6.1f}-{fr/60:6.1f}s): {fr-t0:4d} VI, {dp-dp0:4d} lists, {(dp-dp0)/max(1,(fr-t0))*60:5.1f} fps")
        last_scene=scene; t0=fr; dp0=dp
print(f"(last) scene {last_scene} from VI {t0} ({t0/60:.1f}s), {dp-dp0} lists in {fr-t0} VI")
