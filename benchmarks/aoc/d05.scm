; AoC 2017 day 5 — s7 Scheme port of p05.clj (same input, same algorithm)
; NOTE: idiomatic Scheme uses a mutable vector (vector-set!); the cljc
; original uses a *persistent* vector (assoc) by design as a stress test.
(define (slurp path)
  (call-with-input-file path
    (lambda (port)
      (let loop ((cs '()))
        (let ((c (read-char port)))
          (if (eof-object? c) (list->string (reverse cs)) (loop (cons c cs))))))))

(define (split-lines str)
  (let ((n (string-length str)))
    (let loop ((i 0) (cur '()) (out '()))
      (if (= i n)
          (reverse (if (null? cur) out (cons (list->string (reverse cur)) out)))
          (let ((c (string-ref str i)))
            (if (char=? c #\newline)
                (loop (+ i 1) '() (if (null? cur) out (cons (list->string (reverse cur)) out)))
                (loop (+ i 1) (cons c cur) out)))))))

(define tape
  (list->vector (map string->number (split-lines (slurp "input/05.txt")))))
(define n (vector-length tape))

(define (vcopy v)
  (let ((out (make-vector (vector-length v))))
    (do ((i 0 (+ i 1))) ((= i (vector-length v)) out)
      (vector-set! out i (vector-ref v i)))))

(define (exit-steps bump)
  (let ((t (vcopy tape)))
    (let loop ((loc 0) (step 0))
      (if (or (< loc 0) (>= loc n)) step
          (let ((v (vector-ref t loc)))
            (vector-set! t loc (bump v))
            (loop (+ loc v) (+ step 1)))))))

(display "Answer1: ") (display (exit-steps (lambda (v) (+ v 1)))) (newline)
(display "Answer2: ") (display (exit-steps (lambda (v) (if (>= v 3) (- v 1) (+ v 1))))) (newline)
