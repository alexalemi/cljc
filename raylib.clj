;; raylib.clj — a cljc binding for raylib (https://www.raylib.com), built on the
;; FFI's :float and struct-by-value support.
;;
;; raylib's API is full of `float` arguments and by-value structs (Color,
;; Vector2, Rectangle), so this module is the headline test of those FFI
;; features. Structs cross the boundary as plain cljc vectors:
;;   Color     -> [r g b a]      Vector2   -> [x y]
;;   Rectangle -> [x y w h]
;;
;; Link configuration is machine-specific. Override via env vars; the defaults
;; point at a from-source build (zig-out). For a system install (apt/brew),
;; set:  CLJC_RAYLIB_INC=/usr/include  CLJC_RAYLIB_LIBS="-lraylib -lm"
;;
;; Load with: (require '[raylib :as rl])  or  (load-file "raylib.clj")

(def rl-inc  (or (cljc/env* "CLJC_RAYLIB_INC")
                 "/home/alemi/build/raylib/zig-out/include"))
(def rl-libs (or (cljc/env* "CLJC_RAYLIB_LIBS")
                 (str "/home/alemi/build/raylib/zig-out/lib/libraylib.a "
                      "-fsanitize=undefined -lGL -lm -lpthread -ldl -lrt -lX11")))

(declare InitWindow SetTargetFPS WindowShouldClose CloseWindow SetConfigFlags
         BeginDrawing EndDrawing ClearBackground DrawText DrawFPS
         DrawRectangle DrawRectangleV DrawCircle DrawCircleV DrawLine
         GetScreenWidth GetScreenHeight GetFrameTime GetTime MeasureText
         IsKeyDown IsKeyPressed GetMouseX GetMouseY GetMousePosition
         IsMouseButtonDown TakeScreenshot DrawRectangleLines DrawCircleLines)

(ffi/define
  [[:void   "InitWindow"        [:int :int :string]]
   [:void   "SetTargetFPS"      [:int]]
   [:int    "WindowShouldClose" []]                 ; bool -> 0/1
   [:void   "CloseWindow"       []]
   [:void   "SetConfigFlags"    [:int]]
   [:void   "BeginDrawing"      []]
   [:void   "EndDrawing"        []]
   [:void   "ClearBackground"   ["Color"]]
   [:void   "DrawText"          [:string :int :int :int "Color"]]
   [:void   "DrawFPS"           [:int :int]]
   [:void   "DrawRectangle"     [:int :int :int :int "Color"]]
   [:void   "DrawRectangleV"    ["Vector2" "Vector2" "Color"]]
   [:void   "DrawCircle"        [:int :int :float "Color"]]
   [:void   "DrawCircleV"       ["Vector2" :float "Color"]]
   [:void   "DrawLine"          [:int :int :int :int "Color"]]
   [:int    "GetScreenWidth"    []]
   [:int    "GetScreenHeight"   []]
   [:float  "GetFrameTime"      []]
   [:double "GetTime"           []]
   [:int    "MeasureText"       [:string :int]]
   [:int    "IsKeyDown"         [:int]]
   [:int    "IsKeyPressed"      [:int]]
   [:int    "GetMouseX"         []]
   [:int    "GetMouseY"         []]
   ["Vector2" "GetMousePosition" []]                ; struct RETURN -> [x y]
   [:int    "IsMouseButtonDown" [:int]]
   [:void   "TakeScreenshot"    [:string]]
   [:void   "DrawRectangleLines" [:int :int :int :int "Color"]]
   [:void   "DrawCircleLines"   [:int :int :float "Color"]]]
  {:headers ["raylib.h"]
   :libs    (str "-I" rl-inc " " rl-libs)
   :structs {"Color"     [[:int "r"] [:int "g"] [:int "b"] [:int "a"]]
             "Vector2"   [[:float "x"] [:float "y"]]
             "Rectangle" [[:float "x"] [:float "y"] [:float "width"] [:float "height"]]}})

;; ── color constants (raylib's palette), as [r g b a] vectors ──
(def RAYWHITE [245 245 245 255])
(def WHITE    [255 255 255 255])
(def BLACK    [0 0 0 255])
(def LIGHTGRAY[200 200 200 255])
(def GRAY     [130 130 130 255])
(def DARKGRAY [80 80 80 255])
(def RED      [230 41 55 255])
(def MAROON   [190 33 55 255])
(def GREEN    [0 228 48 255])
(def LIME     [0 158 47 255])
(def BLUE     [0 121 241 255])
(def SKYBLUE  [102 191 255 255])
(def DARKBLUE [0 82 172 255])
(def YELLOW   [253 249 0 255])
(def GOLD     [255 203 0 255])
(def ORANGE   [255 161 0 255])
(def PURPLE   [200 122 255 255])
(def VIOLET   [135 60 190 255])
(def PINK     [255 109 194 255])

;; ── a few key codes (raylib uses GLFW codes) ──
(def KEY_SPACE 32)  (def KEY_ESCAPE 256) (def KEY_ENTER 257)
(def KEY_RIGHT 262) (def KEY_LEFT 263)   (def KEY_DOWN 264) (def KEY_UP 265)
(def KEY_A 65) (def KEY_D 68) (def KEY_S 83) (def KEY_W 87)

;; ── flag bits ──
(def FLAG_VSYNC_HINT 64)
(def FLAG_WINDOW_RESIZABLE 4)

;; ── convenience predicate wrappers (bool comes back as 0/1) ──
(defn should-close? [] (not (zero? (WindowShouldClose))))
(defn key-down?  [k] (not (zero? (IsKeyDown k))))
(defn key-pressed? [k] (not (zero? (IsKeyPressed k))))
