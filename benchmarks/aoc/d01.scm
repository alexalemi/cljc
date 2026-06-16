; AoC 2017 day 1 — s7 Scheme port of p01.clj (same input, same algorithm)
(define (slurp path)
  (call-with-input-file path
    (lambda (port)
      (let loop ((cs '()))
        (let ((c (read-char port)))
          (if (eof-object? c) (list->string (reverse cs)) (loop (cons c cs))))))))

(define data (slurp "input/01.txt"))
(define ds
  (let loop ((i 0) (acc '()))
    (if (>= i (string-length data)) (reverse acc)
        (let ((c (string-ref data i)))
          (loop (+ i 1)
                (if (char-numeric? c) (cons (- (char->integer c) 48) acc) acc))))))
(define dv (list->vector ds))
(define n (vector-length dv))

(define (sum-matching gap)
  (let loop ((i 0) (s 0))
    (if (= i n) s
        (let ((a (vector-ref dv i)) (b (vector-ref dv (modulo (+ i gap) n))))
          (loop (+ i 1) (+ s (if (= a b) a 0)))))))

(display "Answer1: ") (display (sum-matching 1)) (newline)
(display "Answer2: ") (display (sum-matching (quotient n 2))) (newline)
