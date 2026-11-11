using IMao_WinUI.Helpers;

static void Check(bool value, string name) { if (!value) throw new Exception(name); Console.WriteLine("PASS " + name); }
var frame = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
bool started = false;
var preparation = StartupPreparation.RunAsync(() => frame.Task, _ => { started = true; return Task.FromResult(42); }, CancellationToken.None);
Check(!started, "resources wait for the displayed frame");
frame.SetResult();
Check(await preparation == 42, "ready result is returned");

int callerThread = Environment.CurrentManagedThreadId;
// Keep the caller blocked to ensure worker and caller cannot be the same pool thread.
var workerThread = StartupPreparation.RunAsync(() => Task.CompletedTask, _ => Task.FromResult(Environment.CurrentManagedThreadId), CancellationToken.None).GetAwaiter().GetResult();
Check(callerThread != workerThread, "synchronous preparation prefix executes off the caller thread");

using var cancelledBefore = new CancellationTokenSource();
cancelledBefore.Cancel();
bool called = false;
try { await StartupPreparation.RunAsync(() => Task.CompletedTask, _ => { called = true; return Task.FromResult(1); }, cancelledBefore.Token); throw new Exception("Expected cancellation"); }
catch (OperationCanceledException) { Check(!called, "closed window never begins preparation"); }

using var closedBeforeFrame = new CancellationTokenSource();
var missingFrame = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
bool preparedBeforeFrame = false;
var waitingForFrame = StartupPreparation.RunAsync(() => missingFrame.Task, _ => { preparedBeforeFrame = true; return Task.FromResult(1); }, closedBeforeFrame.Token);
closedBeforeFrame.Cancel();
try { await waitingForFrame; throw new Exception("Expected cancellation"); }
catch (OperationCanceledException) { Check(!preparedBeforeFrame, "closing before the first frame cancels without resource work"); }

using var closedDuring = new CancellationTokenSource();
try { await StartupPreparation.RunAsync(() => Task.CompletedTask, _ => { closedDuring.Cancel(); return Task.FromResult(1); }, closedDuring.Token); throw new Exception("Expected cancellation"); }
catch (OperationCanceledException) { Check(true, "close during non-cancellable preparation cannot publish readiness"); }

var failure = new IOException("fixture resource failure");
try { await StartupPreparation.RunAsync<int>(() => Task.CompletedTask, _ => throw failure, CancellationToken.None); throw new Exception("Expected failure"); }
catch (IOException actual) { Check(ReferenceEquals(actual, failure), "resource failures reach the visible error handler unchanged"); }
