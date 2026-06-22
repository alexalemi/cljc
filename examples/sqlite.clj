;; sqlite.clj — talk to a real SQLite database from cljc, through the FFI.
;;
;; Showcases: binding a substantial C library (libsqlite3) with zero glue code
;; checked in. SQLite's API uses out-parameters (sqlite3**) that a scalar FFI
;; can't express, so we emit a handful of `static inline` shim functions into a
;; header and bind those — the standard pattern for adapting a C API to cljc's
;; FFI. Handles travel as integers; the prepare/step/column loop reads rows.
;;
;; Requires libsqlite3-dev (the sqlite3.h header) and a C compiler.
;;
;; Run:  cljc examples/sqlite.clj

;; ── the C shim: flatten sqlite3's out-params into handle-returning calls ──
(def shim "
#include <sqlite3.h>
static inline long long sl_open(const char*p){sqlite3*db=0;return sqlite3_open(p,&db)==SQLITE_OK?(long long)db:0;}
static inline long long sl_prep(long long db,const char*q){sqlite3_stmt*s=0;sqlite3_prepare_v2((sqlite3*)db,q,-1,&s,0);return(long long)s;}
static inline int sl_step(long long s){return sqlite3_step((sqlite3_stmt*)s);}
static inline int sl_ncol(long long s){return sqlite3_column_count((sqlite3_stmt*)s);}
static inline int sl_ctype(long long s,int i){return sqlite3_column_type((sqlite3_stmt*)s,i);}
static inline const char*sl_cname(long long s,int i){return sqlite3_column_name((sqlite3_stmt*)s,i);}
static inline const char*sl_ctext(long long s,int i){return(const char*)sqlite3_column_text((sqlite3_stmt*)s,i);}
static inline int sl_cint(long long s,int i){return sqlite3_column_int((sqlite3_stmt*)s,i);}
static inline double sl_cdbl(long long s,int i){return sqlite3_column_double((sqlite3_stmt*)s,i);}
static inline int sl_final(long long s){return sqlite3_finalize((sqlite3_stmt*)s);}
static inline int sl_exec(long long db,const char*q){return sqlite3_exec((sqlite3*)db,q,0,0,0);}
static inline const char*sl_err(long long db){return sqlite3_errmsg((sqlite3*)db);}
static inline int sl_close(long long db){return sqlite3_close((sqlite3*)db);}
")

(def shim-path "/tmp/cljc_sqlite_shim.h")
(spit shim-path shim)

(declare sl_open sl_prep sl_step sl_ncol sl_ctype sl_cname
         sl_ctext sl_cint sl_cdbl sl_final sl_exec sl_err sl_close)

(ffi/define
  [[:int    "sl_open"  [:string]]      ; → db handle (0 on failure)
   [:int    "sl_prep"  [:int :string]] ; → statement handle
   [:int    "sl_step"  [:int]]         ; 100 = SQLITE_ROW, 101 = SQLITE_DONE
   [:int    "sl_ncol"  [:int]]
   [:int    "sl_ctype" [:int :int]]    ; 1 int, 2 float, 3 text, 5 null
   [:string "sl_cname" [:int :int]]
   [:string "sl_ctext" [:int :int]]
   [:int    "sl_cint"  [:int :int]]
   [:double "sl_cdbl"  [:int :int]]
   [:int    "sl_final" [:int]]
   [:int    "sl_exec"  [:int :string]]
   [:string "sl_err"   [:int]]
   [:int    "sl_close" [:int]]]
  {:headers ["cljc_sqlite_shim.h"] :libs "-I/tmp -lsqlite3"})

;; ── a small Clojure-flavored wrapper over the shim ──
(defn open [path]
  (let [db (sl_open path)]
    (when (zero? db) (throw (ex-info (str "cannot open " path) {})))
    db))

(defn exec! [db sql]
  (when-not (zero? (sl_exec db sql))
    (throw (ex-info (str "sqlite: " (sl_err db)) {:sql sql}))))

;; Run a SELECT and return a vector of row maps {column-name value}, reading
;; each cell by its dynamic column type — the prepare/step/column dance.
(defn query [db sql]
  (let [stmt (sl_prep db sql)
        ncol (sl_ncol stmt)]
    (loop [rows []]
      (if (= 100 (sl_step stmt))                 ; SQLITE_ROW
        (recur (conj rows
                 (into {}
                   (for [i (range ncol)]
                     [(sl_cname stmt i)
                      (case (sl_ctype stmt i)
                        1 (sl_cint stmt i)        ; INTEGER
                        2 (sl_cdbl stmt i)        ; FLOAT
                        3 (sl_ctext stmt i)       ; TEXT
                        nil)]))))                 ; NULL / other
        (do (sl_final stmt) rows)))))

;; ── demo: build a tiny database in memory and query it ──
(let [db (open ":memory:")]
  (exec! db "CREATE TABLE langs (name TEXT, year INTEGER, lines REAL)")
  (doseq [[n y l] [["C" 1972 0.0] ["Clojure" 2007 0.0]
                   ["cljc" 2026 8500.0] ["Janet" 2017 0.0]]]
    (exec! db (format "INSERT INTO langs VALUES ('%s', %d, %f)" n y l)))

  (println "All rows, newest first:")
  (doseq [row (query db "SELECT name, year FROM langs ORDER BY year DESC")]
    (println (format "  %-8s %d" (get row "name") (get row "year"))))

  (println "\nAggregate query:")
  (let [r (first (query db "SELECT COUNT(*) AS n, MIN(year) AS oldest FROM langs"))]
    (println (format "  %d languages, oldest from %d" (get r "n") (get r "oldest"))))

  (println "\nParameterized-ish filter (year > 2000):")
  (doseq [row (query db "SELECT name FROM langs WHERE year > 2000 ORDER BY name")]
    (println "  -" (get row "name")))

  (sl_close db)
  (println "\nDatabase closed. (This ran against real libsqlite3 via the FFI.)"))
