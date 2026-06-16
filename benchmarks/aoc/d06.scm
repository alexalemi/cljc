; AoC 2017 day 6 — s7 Scheme port of p06.clj (same input, same algorithm)
; Banks are lists; seen-states keyed by the list value (equal? hashing).
(define (slurp path)
  (call-with-input-file path
    (lambda (port)
      (let loop ((cs '()))
        (let ((c (read-char port)))
          (if (eof-object? c) (list->string (reverse cs)) (loop (cons c cs))))))))

; pull all integers out of a string
(define (read-ints str)
  (let ((n (string-length str)))
    (let loop ((i 0) (cur '()) (out '()))
      (define (flush) (if (null? cur) out (cons (string->number (list->string (reverse cur))) out)))
      (if (= i n)
          (reverse (flush))
          (let ((c (string-ref str i)))
            (if (char-numeric? c)
                (loop (+ i 1) (cons c cur) out)
                (loop (+ i 1) '() (flush))))))))

(define data (list->vector (read-ints (slurp "input/06.txt"))))
(define n (vector-length data))

(define (max-index banks)
  (let loop ((i 0) (bi 0) (bv -1))
    (if (= i n) bi
        (let ((v (vector-ref banks i)))
          (if (> v bv) (loop (+ i 1) i v) (loop (+ i 1) bi bv))))))

(define (redistribute banks)
  (let ((b (vcopy banks)) (idx (max-index banks)))
    (let ((blocks (vector-ref b idx)))
      (vector-set! b idx 0)
      (let loop ((i (modulo (+ idx 1) n)) (k blocks))
        (if (= k 0) b
            (begin (vector-set! b i (+ 1 (vector-ref b i)))
                   (loop (modulo (+ i 1) n) (- k 1))))))))

(define (vcopy v)
  (let ((out (make-vector (vector-length v))))
    (do ((i 0 (+ i 1))) ((= i (vector-length v)) out)
      (vector-set! out i (vector-ref v i)))))

(define (key b) (vector->list b))

(define (run banks)
  (let ((seen (make-hash-table)))
    (let loop ((b banks) (c 0))
      (let ((k (key b)))
        (let ((prev (hash-table-ref seen k)))
          (if prev
              (cons (- c prev) c)              ; (cycle-length . steps-to-first-repeat)
              (begin (hash-table-set! seen k c)
                     (loop (redistribute b) (+ c 1)))))))))

(let ((r (run data)))
  (display "Answer1: ") (display (cdr r)) (newline)
  (display "Answer2: ") (display (car r)) (newline))
