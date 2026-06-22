;; raylib-bounce.clj — an interactive GUI demo: bouncing balls you can scatter
;; with the mouse. Driven entirely from cljc through the FFI raylib binding.
;;
;; Showcases: the FFI's :float + struct-by-value support (every draw call passes
;; a Color [r g b a] and Vector2 [x y] by value), immutable game state threaded
;; through a frame loop, and reading live input.
;;
;; PREREQUISITES
;;   - raylib installed (header + lib). Configure paths if not in the defaults:
;;       CLJC_RAYLIB_INC=/usr/include  CLJC_RAYLIB_LIBS="-lraylib -lm"
;;   - a display. Locally that's your desktop. Headless / over SSH / tailscale,
;;     run under a virtual framebuffer so it doesn't need a real screen:
;;       xvfb-run -s "-screen 0 800x600x24" cljc examples/raylib-bounce.clj --shot
;;     (--shot renders a few frames and writes raylib-bounce.png you can view
;;     or copy back; without it, the window runs interactively.)
;;
;; Run:  cljc examples/raylib-bounce.clj          ; interactive window
;;       cljc examples/raylib-bounce.clj --shot    ; render + screenshot, then exit

;; raylib.clj is a battery in the repo ROOT, but this example lives in
;; examples/. require searches *load-path* (which includes "."), so add ".." so
;; the binding is found whether you run from the repo root OR from examples/.
(def *load-path* (into [".." "."] *load-path*))
(require '[raylib :as rl])

;; If the binding didn't load (raylib.clj not on the path), require silently
;; no-ops in cljc's flat-global model — so probe and fail with a clear message
;; instead of a baffling "unknown rl/RED".
(when-not (try (fn? rl/InitWindow) (catch Exception _ false))
  (throw (ex-info (str "couldn't load raylib.clj — run from the cljc repo root or "
                       "its examples/ dir (e.g. `cljc examples/raylib-bounce.clj`), "
                       "or set CLJC_PATH to the directory containing raylib.clj.") {})))

(def W 800)
(def H 600)
(def palette [rl/RED rl/GREEN rl/BLUE rl/GOLD rl/PURPLE rl/ORANGE rl/SKYBLUE rl/PINK])

;; A ball is a map. Pure step function: move, then reflect velocity at walls.
(defn step-ball [{:keys [x y vx vy r color] :as b}]
  (let [nx (+ x vx) ny (+ y vy)
        vx (if (or (< nx r) (> nx (- W r))) (- vx) vx)
        vy (if (or (< ny r) (> ny (- H r))) (- vy) vy)]
    (assoc b :x (+ x vx) :y (+ y vy) :vx vx :vy vy)))

(defn make-ball [i]
  {:x (+ 100 (* 70 (mod i 9)))
   :y (+ 100 (* 60 (mod i 7)))
   :vx (- (rem (* (inc i) 37) 9) 4)
   :vy (- (rem (* (inc i) 53) 9) 4)
   :r (+ 12.0 (rem (* i 13) 24))
   :color (nth palette (mod i (count palette)))})

(defn draw-frame [balls]
  (rl/BeginDrawing)
  (rl/ClearBackground [20 24 32 255])
  (doseq [{:keys [x y r color]} balls]
    (rl/DrawCircleV [(* 1.0 x) (* 1.0 y)] (* 1.0 r) color)
    (rl/DrawCircleLines (int x) (int y) (* 1.0 r) rl/RAYWHITE))
  (rl/DrawText "cljc + raylib  —  click to scatter, ESC to quit" 20 20 20 rl/RAYWHITE)
  (rl/DrawFPS (- W 90) 20)
  (rl/EndDrawing))

(let [shot? (some #(= % "--shot") *args*)]
  (rl/InitWindow W H "cljc bouncing balls")
  (rl/SetTargetFPS 60)
  (loop [balls (mapv make-ball (range 24))
         frame 0]
    (let [;; a mouse click gives every ball a fresh random-ish kick
          balls (if (and (not shot?) (not (zero? (rl/IsMouseButtonDown 0))))
                  (mapv (fn [b] (assoc b :vx (- (rem (* frame 7) 11) 5)
                                         :vy (- (rem (* frame 5) 11) 5)))
                        balls)
                  balls)]
      (draw-frame balls)
      (cond
        ;; screenshot mode: capture a frame, then stop
        (and shot? (= frame 30))
        (do (rl/TakeScreenshot "raylib-bounce.png")
            (println "wrote raylib-bounce.png"))
        ;; interactive: run until the window closes or ESC
        (and (not shot?) (rl/should-close?))
        nil
        :else
        (recur (mapv step-ball balls) (inc frame)))))
  (rl/CloseWindow))
