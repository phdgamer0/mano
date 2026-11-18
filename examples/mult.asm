        ORG 100
LOOP,   LDA SUM
        ADD M
        STA SUM
        ISZ CNT
        BUN LOOP
        LDA SUM
        OUT
        HLT
M,      DEC 6
N,      DEC 7
CNT,    DEC -7
SUM,    DEC 0
        END
