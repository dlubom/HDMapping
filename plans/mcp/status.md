# Status MCP

Plan zaakceptowany. P00 wykonano częściowo; wynik **INCOMPLETE**. Właściciel 2026-11-16 polecił odłożyć kwalifikację Windows, Linux i klienta Claude oraz przejść dalej: **P01 rozpoczęto mimo niepełnego P00; implementacja i dostępne kontrole macOS są dostarczane jako INCOMPLETE**. Szczegóły, dowody i zakres tej decyzji: [raport P00](results/P00.md#decyzja-właściciela-po-raporcie).

Uzgodniony [standard jakości Go/C++](quality.md) jest częścią planu: P00 kwalifikuje narzędzia, P01 wdraża kontrole Go, P03 obejmuje nimi nowe moduły C++. Wdrożone kontrole Go i ich wykonany zakres opisuje raport P01; samo dodanie konfiguracji nie zalicza pełnej macierzy.

- Repozytorium do zapisu: `dlubom/HDMapping`.
- Branch planu: `mcp/implementation-plan`.
- Bazowy commit rozpoznania: `3d3a8054986f95fcf580ef0f780889043bfdd475`.
- Baza P01: `8d8ec2a15099e13dba5237b16a5e70a2b324b2fd`; badany kod P01: `0707dd20d05a61112429a73d799a21ab3d36f828`.
- Branch kwalifikacji: `mcp/p00`.
- Następna praca: **kontynuacja P01 na `mcp/p01`** od zdalnego SHA z przekazania. [Raport P01](results/P01.md): minimalne stdio, Go/CMake/install/CI; pełny odbiór platform i wiki pozostają otwarte. P02 nie rozpoczęto.
- Braki: Windows 11, Claude Desktop, pełny test klienta/GUI Ubuntu, działający Linux TSan, potwierdzone nakładanie i bazowy projekt kroku 3 oraz zapis/odczyt wyniku optymalizacji GUI kroku 2.
- Odroczenie nie zmienia wyników kontroli ani wymagań przenośności i nie upoważnia do usuwania istniejących kontroli CI. Braki danych pozostają otwarte; nie są dowodem zgodności przyszłych zmian przetwarzania.
- Kolejne czaty stosują [zasady przekazania](README.md).

| Karta | Stan | Raport |
| --- | --- | --- |
| P00 | INCOMPLETE | [P00](results/P00.md) |
| P01 | INCOMPLETE | [P01](results/P01.md) |
| P02 | NOT STARTED | — |
| P03 | NOT STARTED | — |
| P04 | NOT STARTED | — |
| P05 | NOT STARTED | — |
| P06 | NOT STARTED | — |
| P07 | NOT STARTED | — |
| P08 | NOT STARTED | — |
| P09 | NOT STARTED | — |
| P10 | NOT STARTED | — |
| P11 | NOT STARTED | — |
| P12 | NOT STARTED | — |
| P13 | NOT STARTED | — |
| P14 | NOT STARTED | — |
| P15 | NOT STARTED | — |
| P16 | NOT STARTED | — |
| P17 | NOT STARTED | — |

Dozwolone stany: `NOT STARTED`, `IN PROGRESS`, `INCOMPLETE`, `PASSED`. Raport musi uzasadniać zmianę stanu. Sam commit, push lub ukończenie części sprawdzeń nie oznacza zaliczenia karty.
