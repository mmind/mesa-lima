VERT

DCL IN[0]
DCL IN[1]
DCL OUT[0], POSITION
DCL OUT[1], COLOR

IMM FLT32 { 0.1, 0.1, 0.0, 0.0 }

SUB OUT[0], IN[0], IMM[0]
MOV OUT[1], IN[1]

END
