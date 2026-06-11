;; AoC 2017 day 4 — portable across cljc / babashka / clojure.
(try (require (quote [clojure.string :as str])) (catch Exception e nil)) ; no-op on cljc
(def lines (str/split-lines (slurp "input/04.txt")))

(defn wordlist [line] (re-seq #"\w+" line))

(defn no-dups? [ws] (= (count ws) (count (set ws))))

(def ans1 (count (filter (fn [l] (no-dups? (wordlist l))) lines)))

(defn no-anagrams? [ws]
  (= (count ws) (count (set (map (fn [w] (frequencies (seq w))) ws)))))

(def ans2 (count (filter (fn [l] (no-anagrams? (wordlist l))) lines)))

(println "Answer1:" ans1)
(println "Answer2:" ans2)
