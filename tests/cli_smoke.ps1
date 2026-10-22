param(
    [Parameter(Mandatory = $true)]
    [string] $BuildDirectory
)

$ErrorActionPreference = 'Stop'
$build = (Resolve-Path $BuildDirectory).Path
$cli = Join-Path $build 'knock.exe'
$fixture = Join-Path $build 'runner_fixture.exe'
if (-not (Test-Path $cli) -or -not (Test-Path $fixture)) {
    throw 'Build knock.exe and runner_fixture.exe before running this test.'
}
if (Get-Process KnockTray -ErrorAction SilentlyContinue) {
    throw 'Exit Knock from its tray menu before running this isolated smoke test.'
}
$previousData = $env:KNOCK_DATA_DIR
$testData = Join-Path $env:TEMP ('knock-smoke-' + [Guid]::NewGuid().ToString('N'))
$env:KNOCK_DATA_DIR = $testData

function Invoke-Knock {
    param([string[]] $Arguments, [int] $ExpectedExit = 0)
    $raw = & $cli @Arguments
    $actualExit = $LASTEXITCODE
    if ($actualExit -ne $ExpectedExit) {
        throw "Exit code $actualExit, expected $ExpectedExit. Output: $raw"
    }
    return ($raw | ConvertFrom-Json)
}

function Assert-Equal {
    param($Actual, $Expected, [string] $Description)
    if ($Actual -cne $Expected) { throw "$Description`: expected '$Expected', got '$Actual'" }
}

function Wait-JobResult {
    param([string] $Id)
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $result = Invoke-Knock -Arguments @('job', $Id)
        if ($result.job.status -ne 'running') { return $result.job }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Job $Id did not finish."
}

try {
    $null = Invoke-Knock -Arguments @('request', '--message', 'Missing chat') -ExpectedExit 5
    $state = Invoke-Knock -Arguments @('status')
    Assert-Equal $state.requests_pending 0 'Fresh request list'
    $null = Invoke-Knock -Arguments @('notes', 'add', '--title', 'First', '--title', 'Duplicate') -ExpectedExit 5
    $null = Invoke-Knock -Arguments @('notes', 'add', '--title', 'Invalid', '--clear-tags') -ExpectedExit 5
    $null = Invoke-Knock -Arguments @('tasks', 'add', '--title', 'Invalid', '--tag', 'Ideas') -ExpectedExit 5
    $note = Invoke-Knock -Arguments @('notes', 'add', '--title', 'Local smoke note', '--body', 'First line', '--tag', ' Ideas ', '--tag', 'ideas', '--tag', 'Personal projects')
    Assert-Equal ($note.note.tags -join '|') 'Ideas|Personal projects' 'Repeated tags and normalization'
    $null = Invoke-Knock -Arguments @('notes', 'update', $note.note.id, '--body', 'Saved edit')
    $notes = Invoke-Knock -Arguments @('notes', 'list')
    Assert-Equal $notes.notes[0].body 'Saved edit' 'Note update'
    Assert-Equal ($notes.notes[0].tags -join '|') 'Ideas|Personal projects' 'Body edit preserves tags'
    $cleared = Invoke-Knock -Arguments @('notes', 'update', $note.note.id, '--clear-tags')
    Assert-Equal (@($cleared.note.tags).Count) 0 'Clear tags'
    $tagged = Invoke-Knock -Arguments @('notes', 'update', $note.note.id, '--tag', 'Personal')
    Assert-Equal ($tagged.note.tags -join '|') 'Personal' 'Replace tags'
    $null = Invoke-Knock -Arguments @('notes', 'update', $note.note.id, '--clear-tags', '--tag', 'Invalid') -ExpectedExit 5

    $conversation = Invoke-Knock -Arguments @('notes', 'get', $note.note.id)
    Assert-Equal (@($conversation.note.messages).Count) 0 'Description is not a sent message'
    Assert-Equal $conversation.note.body 'Saved edit' 'Full conversation description'
    $firstMessage = Invoke-Knock -Arguments @('notes', 'append', $note.note.id, '--body', 'https://example.com/ - a useful reference')
    $messageText = "Second message`nUnicode " + [char]0x2713
    $messageFile = Join-Path $testData 'message.txt'
    [System.IO.File]::WriteAllText($messageFile, $messageText, [System.Text.Encoding]::UTF8)
    $secondMessage = Invoke-Knock -Arguments @('notes', 'append', $note.note.id, '--body-file', $messageFile)
    Assert-Equal $secondMessage.message.text $messageText 'Unicode message file and BOM'
    $firstPage = Invoke-Knock -Arguments @('notes', 'get', $note.note.id, '--limit', '1')
    Assert-Equal $firstPage.total_messages 2 'Conversation message count'
    Assert-Equal $firstPage.offset 0 'First message offset'
    Assert-Equal $firstPage.next_offset 1 'Conversation continuation offset'
    Assert-Equal $firstPage.note.messages[0].id $firstMessage.message.id 'Oldest message first'
    $lastPage = Invoke-Knock -Arguments @('notes', 'get', $note.note.id, '--offset', '1', '--limit', '1')
    Assert-Equal $lastPage.next_offset $null 'Final conversation page'
    Assert-Equal $lastPage.note.messages[0].id $secondMessage.message.id 'Second message identity'
    $metadata = Invoke-Knock -Arguments @('notes', 'update', $note.note.id, '--body', 'Saved edit')
    Assert-Equal $metadata.note.message_count 2 'Description edit preserves conversation'
    if ($metadata.note.PSObject.Properties.Name -contains 'messages') { throw 'Metadata returned the entire conversation.' }
    $null = Invoke-Knock -Arguments @('notes', 'append', $note.note.id, '--body', ' ') -ExpectedExit 5
    $null = Invoke-Knock -Arguments @('notes', 'append', $note.note.id, '--body', 'text', '--body-file', $messageFile) -ExpectedExit 5
    $null = Invoke-Knock -Arguments @('notes', 'get', $note.note.id, '--limit', '101') -ExpectedExit 5
    $conversation = Invoke-Knock -Arguments @('notes', 'get', $note.note.id)
    Assert-Equal $conversation.total_messages 2 'Invalid append preserves conversation'

    $task = Invoke-Knock -Arguments @('tasks', 'add', '--title', 'Smoke task', '--operator', 'TEST-1', '--project', 'KNOCK')
    $null = Invoke-Knock -Arguments @('tasks', 'update', $task.task.id, '--status', 'in_progress')
    $tasks = Invoke-Knock -Arguments @('tasks', 'list')
    Assert-Equal $tasks.tasks[0].status 'in_progress' 'Task status'

    $request = Invoke-Knock -Arguments @('request', '--chat', 'TEST-1', '--message', 'Do not approve this automated test.', '--detach')
    $pending = Invoke-Knock -Arguments @('result', $request.request.id) -ExpectedExit 7
    Assert-Equal $pending.request.status 'pending' 'Pending is not approval'
    $null = Invoke-Knock -Arguments @('cancel', $request.request.id)
    $cancelled = Invoke-Knock -Arguments @('result', $request.request.id) -ExpectedExit 6
    Assert-Equal $cancelled.request.status 'cancelled' 'Cancellation'
    $expired = Invoke-Knock -Arguments @('request', '--chat', 'TEST-1', '--kind', 'question', '--message', 'Timeout test', '--timeout', '1') -ExpectedExit 4
    Assert-Equal $expired.request.status 'expired' 'Timeout does not approve'

    $run = Invoke-Knock -Arguments @('run', '--chat', 'TEST-1', '--title', 'Output test', '--', $fixture, 'echo', 'space value', 'literal & | > < ;')
    $job = Wait-JobResult -Id $run.job.id
    Assert-Equal $job.exit_code 0 'Command exit code'
    if ($job.output_tail -notlike '*stderr-captured*') { throw 'stderr was not captured.' }
    $shell = Invoke-Knock -Arguments @('run', '--chat', 'TEST-1', '--title', 'Explicit shell syntax', '--', 'cmd.exe', '/d', '/s', '/c', 'echo shell-syntax-ok')
    $job = Wait-JobResult -Id $shell.job.id
    Assert-Equal $job.exit_code 0 'Explicit cmd command'
    if ($job.output_tail -notlike '*shell-syntax-ok*') { throw 'cmd command text was not preserved.' }
    $flood = Invoke-Knock -Arguments @('run', '--chat', 'TEST-1', '--title', 'Bounded log test', '--', $fixture, 'flood')
    $job = Wait-JobResult -Id $flood.job.id
    Assert-Equal $job.exit_code 23 'Nonzero exit code'
    if ($job.output_tail -notlike '*TAIL_SENTINEL*') { throw 'Output tail was lost after log cap.' }

    $survivor = Invoke-Knock -Arguments @('run', '--chat', 'TEST-1', '--title', 'Independent worker test', '--', $fixture, 'wait')
    Get-Process KnockTray | Stop-Process -Force
    Start-Sleep -Seconds 3
    $job = Wait-JobResult -Id $survivor.job.id
    Assert-Equal $job.status 'completed' 'Worker survives tray exit'
    $notes = Invoke-Knock -Arguments @('notes', 'list')
    Assert-Equal $notes.notes[0].body 'Saved edit' 'Note survives restart'
    Assert-Equal ($notes.notes[0].tags -join '|') 'Personal' 'Note tags survive restart'
    Assert-Equal $notes.notes[0].message_count 2 'Conversation count survives restart'
    $conversation = Invoke-Knock -Arguments @('notes', 'get', $note.note.id)
    Assert-Equal $conversation.note.messages[0].id $firstMessage.message.id 'First message survives restart'
    Assert-Equal $conversation.note.messages[1].text $messageText 'Unicode message survives restart'

    Write-Host 'CLI smoke checks passed.'
    Write-Host "Test data: $testData"
} finally {
    Get-Process KnockTray -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $build 'KnockTray.exe') } | Stop-Process -Force
    $env:KNOCK_DATA_DIR = $previousData
}
