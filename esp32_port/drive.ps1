# Drive Driver 2 on the ESP32-S3 from the PC keyboard.
#
# A serial console only transmits key PRESSES, so it can tap a button but never
# hold and release one - no good for a throttle or a steering wheel. This reads
# the real keyboard state (down AND up) and sends the complete PSX pad word to
# the board 50 times a second as "=HHHH". The firmware latches it and releases
# everything if the stream stops, so closing this window cannot leave the
# throttle stuck on.
#
#   .\drive.ps1                 # defaults to COM6
#   .\drive.ps1 -Port COM7
#   .\drive.ps1 -Quiet          # do not echo the board's log output

param(
    [string]$Port = "COM6",
    [switch]$Quiet
)

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Kbd {
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
    public static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
    public static bool Focused() { return GetForegroundWindow() == GetConsoleWindow(); }
}
'@

# PSX pad bits, matching the word the game builds as (buttons[0]<<8)|buttons[1]
$PAD = @{
    L2 = 0x0001; R2 = 0x0002; L1 = 0x0004; R1 = 0x0008
    TRIANGLE = 0x0010; CIRCLE = 0x0020; CROSS = 0x0040; SQUARE = 0x0080
    SELECT = 0x0100; START = 0x0800
    UP = 0x1000; RIGHT = 0x2000; DOWN = 0x4000; LEFT = 0x8000
}

# virtual key -> pad bit. Several keys may map to the same button.
$MAP = @(
    @{ vk = 0x25; bit = $PAD.LEFT     }   # Left arrow   - steer left
    @{ vk = 0x27; bit = $PAD.RIGHT    }   # Right arrow  - steer right
    @{ vk = 0x26; bit = $PAD.UP       }   # Up arrow     - menu up
    @{ vk = 0x28; bit = $PAD.DOWN     }   # Down arrow   - menu down
    @{ vk = 0x41; bit = $PAD.LEFT     }   # A
    @{ vk = 0x44; bit = $PAD.RIGHT    }   # D
    @{ vk = 0x57; bit = $PAD.UP       }   # W
    @{ vk = 0x53; bit = $PAD.DOWN     }   # S
    @{ vk = 0x58; bit = $PAD.CROSS    }   # X            - accelerate / confirm
    @{ vk = 0x5A; bit = $PAD.SQUARE   }   # Z            - brake / reverse
    @{ vk = 0x20; bit = $PAD.TRIANGLE }   # Space        - handbrake
    @{ vk = 0x43; bit = $PAD.CIRCLE   }   # C            - wheelspin
    @{ vk = 0x10; bit = $PAD.L1       }   # Shift        - fast steer
    @{ vk = 0x11; bit = $PAD.R1       }   # Ctrl
    @{ vk = 0x0D; bit = $PAD.START    }   # Enter
    @{ vk = 0x08; bit = $PAD.SELECT   }   # Backspace
)

# single-shot debug commands the firmware understands (see esp_platform.cpp)
$DUMP = @{ 0x50 = 'p'; 0x56 = 'v'; 0x54 = 't'; 0x52 = 'r' }   # P V T R

Write-Host ""
Write-Host "  Driver 2 - ESP32-S3 keyboard control on $Port" -ForegroundColor Cyan
Write-Host "  ----------------------------------------------"
Write-Host "   arrows / WASD   steer and navigate menus"
Write-Host "   X               accelerate  (also confirms in menus)"
Write-Host "   Z               brake / reverse"
Write-Host "   Space           handbrake        C   wheelspin"
Write-Host "   Shift           fast steer       Ctrl R1"
Write-Host "   Enter  START    Backspace SELECT"
Write-Host "   P V T R         dump screen / VRAM / texture page / raw VRAM"
Write-Host "   Esc             quit"
Write-Host ""
Write-Host "  Keys only register while THIS window has focus." -ForegroundColor DarkGray
Write-Host ""

$sp = New-Object System.IO.Ports.SerialPort($Port, 115200)
$sp.ReadTimeout = 0
$sp.WriteTimeout = 500
try { $sp.Open() } catch { Write-Host "  cannot open $Port : $_" -ForegroundColor Red; exit 1 }

$dumpWasDown = @{}
try {
    while ($true) {
        if ([Kbd]::Focused()) {
            if ([Kbd]::Down(0x1B)) { break }               # Esc

            $mask = 0
            foreach ($m in $MAP) { if ([Kbd]::Down($m.vk)) { $mask = $mask -bor $m.bit } }

            # debug dumps fire once per press, not continuously
            foreach ($vk in $DUMP.Keys) {
                $down = [Kbd]::Down($vk)
                if ($down -and -not $dumpWasDown[$vk]) { $sp.Write($DUMP[$vk]) }
                $dumpWasDown[$vk] = $down
            }
        } else {
            $mask = 0                                       # not focused: hands off
        }

        # resend even when unchanged: the firmware latch times out on silence
        $sp.Write("=" + $mask.ToString("x4"))

        if (-not $Quiet) {
            $in = $sp.ReadExisting()
            if ($in) { Write-Host -NoNewline $in }
        }
        Start-Sleep -Milliseconds 20                        # 50 Hz
    }
} finally {
    try { $sp.Write("=0000"); Start-Sleep -Milliseconds 50; $sp.Close() } catch {}
    Write-Host "`n  released the pad, port closed." -ForegroundColor DarkGray
}
