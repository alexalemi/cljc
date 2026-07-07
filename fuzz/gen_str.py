import random, sys
seed = int(sys.argv[1]); n = int(sys.argv[2]); random.seed(seed)

ASCII = "abcXYZ 019_-.,;!?"
BMP2  = "éüñßøåÆçπλΩ"          # 2-byte
BMP3  = "→∞≠帝国語한글日本"      # 3-byte
AST   = "𝄞𝕊😀🎈"                # 4-byte (astral)

def rstr(charsets, maxlen=12):
    pool = "".join(charsets)
    return "".join(random.choice(pool) for _ in range(random.randint(0, maxlen)))

def clj_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'

# regex-safe literal chars (no metachars)
RXLIT = "abcxyz01é→ñ"
def rre():
    parts = []
    for _ in range(random.randint(1, 3)):
        r = random.random()
        if r < 0.4: parts.append(random.choice(RXLIT))
        elif r < 0.55: parts.append(".")
        elif r < 0.7: parts.append("[" + "".join(random.sample(RXLIT, random.randint(1,3))) + "]")
        elif r < 0.8: parts.append(random.choice(RXLIT) + random.choice("*+?"))
        elif r < 0.9: parts.append("(" + random.choice(RXLIT) + "|" + random.choice(RXLIT) + ")")
        else: parts.append("[^" + random.choice(RXLIT) + "]")
    return "".join(parts)

def gen_bmp(out):
    """differential corpus: BMP-only (Java string indexing agrees below U+10000)"""
    for _ in range(n):
        s = rstr([ASCII, BMP2, BMP3]); sub = rstr([ASCII, BMP2], 3)
        S, SUB = clj_str(s), clj_str(sub)
        L = len(s)  # python len == codepoint count for BMP
        i = random.randint(0, max(0, L - 1)); j = random.randint(i, L)
        re = rre()
        exprs = [
            f'(count {S})',
            f'(subs {S} {min(i, L)} {j})',
            f'(clojure.string/index-of {S} {SUB})',
            f'(clojure.string/last-index-of {S} {SUB})',
            f'(clojure.string/reverse {S})',
            f'(vec (seq {S}))',
            f'(clojure.string/split {S} #"{re}")' if '"' not in re else f'(count {S})',
            f'(clojure.string/replace {S} #"{re}" "_")' if '"' not in re else f'(count {S})',
            f'(re-seq #"{re}" {S})' if '"' not in re else f'(count {S})',
            f'(clojure.string/includes? {S} {SUB})',
            f'(clojure.string/starts-with? {S} {SUB})',
        ]
        if L: exprs.append(f'(nth {S} {i})')
        out.write(random.choice(exprs) + "\n")

def gen_inv(out):
    """self-consistency corpus: any bytes incl. astral; cljc-only asserts"""
    for _ in range(n):
        s = rstr([ASCII, BMP2, BMP3, AST]); S = clj_str(s)
        invs = [
            f'(= {S} (apply str (seq {S})))',
            f'(= (count {S}) (count (vec {S})))',
            f'(= {S} (subs {S} 0 (count {S})))',
            f'(= {S} (clojure.string/reverse (clojure.string/reverse {S})))',
            f'(= {S} (clojure.string/join "" (clojure.string/split {S} #"(?!x)x")))',
            f'(= (count {S}) (count (clojure.string/split {S} #"")))'
            if s else f'(= {S} {S})',
            f'(let [v (vec {S})] (every? true? (map-indexed (fn [i c] (= c (nth {S} i))) v)))',
            f'(let [i (clojure.string/index-of {S} {clj_str(s[len(s)//2:len(s)//2+1]) if s else clj_str("q")})] (or (nil? i) (clojure.string/starts-with? (subs {S} i) {clj_str(s[len(s)//2:len(s)//2+1]) if s else clj_str("q")})))',
        ]
        out.write(random.choice(invs) + "\n")

mode = sys.argv[3]
w = sys.stdout
(gen_bmp if mode == "bmp" else gen_inv)(w)
