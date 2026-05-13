function Check-Visemes {
    $animationJson = Get-Content "src/Animation/example_animation.json" -Raw | ConvertFrom-Json
    $faceJson = Get-Content "src/Morph/universal_face.json" -Raw | ConvertFrom-Json
    
    $morphNames = $faceJson.morphs | ForEach-Object { $_.name }
    $usedVisemes = $animationJson.auto_link | Get-Member -MemberType NoteProperty | Where-Object { $_.Name -like "vrc_v_*" } | ForEach-Object { $_.Name }
    
    $missing = $usedVisemes | Where-Object { $morphNames -notcontains $_ }
    
    if ($missing) {
        Write-Output "Check 1: FAIL"
        Write-Output "Missing visemes: $($missing -join ', ')"
    } else {
        Write-Output "Check 1: PASS"
    }
}

function Check-Order {
    $content = Get-Content "src/Animation/JsonDrivenProtogenAnimation.h" -Raw
    
    # Match the Initialize method starting at line 1113 approximately
    # We use a pattern that matches bool Initialize(...) followed by a block
    $initMatch = [regex]::Match($content, 'bool\s+Initialize\s*\([^)]*\)\s*\{([\s\S]*?)\n\s*\}', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    
    if (-not $initMatch.Success) {
        # Fallback if the block is large and regex hits limits or fails to find end brace easily
        $startPos = $content.IndexOf("bool Initialize")
        if ($startPos -eq -1) {
             Write-Output "Check 2: FAIL (Initialize() not found)"
             return
        }
        $body = $content.Substring($startPos, 5000) # Take a large chunk
    } else {
        $body = $initMatch.Groups[1].Value
    }
    
    $posLoad = $body.IndexOf("LoadAnimationConfig(config)")
    $posAuto = $body.IndexOf("AutoLinkMorphs()")
    $posLink = $body.IndexOf("LinkParameters()")
    
    if ($posLoad -eq -1) {
        Write-Output "Check 2: FAIL (LoadAnimationConfig not found)"
    } elseif ($posAuto -eq -1 -or $posLink -eq -1) {
        Write-Output "Check 2: FAIL (AutoLinkMorphs or LinkParameters not found)"
    } elseif ($posLoad -lt $posAuto -and $posLoad -lt $posLink) {
        Write-Output "Check 2: PASS"
    } else {
        Write-Output "Check 2: FAIL (Incorrect order: LoadAnimationConfig must come first)"
        Write-Output "Positions: Load=$posLoad, Auto=$posAuto, Link=$posLink"
    }
}

Check-Visemes
Check-Order
