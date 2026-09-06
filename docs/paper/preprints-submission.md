# Preprints.org submission — field-by-field

Upload `docs/paper/ringio-paper.pdf` (7 pages). Preprints.org takes the PDF directly; no LaTeX
source, no endorsement, no institutional affiliation required. Everything below is plain text,
ready to paste — the paper's own abstract carries LaTeX escapes that paste badly into a web form.

## Title

SQPOLL io_uring Against a Device-Saturated Cloud NVMe SSD: An Empirical Study

## Author

Parth Kumar Sinha — Independent Researcher, Oxford, United Kingdom
sinhaparth555@gmail.com
ORCID: 0009-0002-3120-9301
Corresponding author: yes

## Subject category

Computer Science and Mathematics → Hardware and Architecture
(second choice: → Software, or → Computer Networks and Communications if neither is offered)

## Keywords

io_uring; SQPOLL; kernel-bypass I/O; NVMe; storage engine; asynchronous I/O; benchmarking

## Abstract (short form, ~200 words — use this one)

Kernel-bypass storage designs built on Linux io_uring's IORING_SETUP_SQPOLL mode rest on a simple
argument: a dedicated kernel polling thread removes the per-operation syscall that other
asynchronous I/O interfaces pay, so on modern NVMe hardware SQPOLL should win on throughput. We
built ringio, a header-only C++20 storage engine, around this argument and tested it against
libaio and plain io_uring on a virtualized cloud NVMe device across two machine sizes (4 and 16
vCPUs) and a swept queue-depth and thread-count matrix. The argument does not hold once the device
saturates. At matched thread count and queue depth, SQPOLL never beats the syscall-based baselines
on raw IOPS at either machine size, and below the device's roughly 200K-IOPS ceiling the gap is
starker: at a single thread libaio reaches more than double SQPOLL's throughput, a gap that widens
on the wider machine. SQPOLL's tail latency also degrades with more available cores. What it does
deliver is roughly an order of magnitude fewer kernel entries per completed operation. A sweep of
sq_thread_idle across four orders of magnitude changes nothing, ruling that parameter out: the
poller never reaches its idle timeout under this workload. We report the full methodology,
including a page-cache defect that invalidated an earlier round of results.

## Abstract (full form, 351 words)

Use the paper's own abstract verbatim if the form accepts it — extract it from the PDF's first
page. The short form above exists because MDPI-family forms often cap the field near 200 words.

## Declarations (the form asks for each separately)

Funding: This work received no external funding. Cloud instances were self-funded.

Conflicts of interest: The author declares no conflict of interest.

Data availability: The engine, the benchmark harness, and the phase-by-phase record of every run
reported, including the configurations behind each table, are at
https://github.com/sinhaparth5/ringio. Raw Google Benchmark JSON output is not archived; the
harness regenerates it from the commands in Section IV.

Ethics / IRB: Not applicable — no human or animal subjects.

Use of AI tools: Claude Code was used for the benchmark harness implementation, run orchestration,
and manuscript preparation. The author verified all reported measurements.

## Notes

- Preprints.org assigns a DOI and screens within roughly 24 hours; screening checks scope and
  plagiarism, not novelty or correctness.
- Posting here does not block a later venue submission. IEEE, USENIX, and ACM all permit
  preprints; check the specific venue's policy before submitting the same manuscript.
- These same declarations are in the PDF's Declarations section, so the two agree.
