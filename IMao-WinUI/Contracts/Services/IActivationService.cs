namespace IMao_WinUI.Contracts.Services;

public interface IActivationService
{
    Task ActivateAsync(object activationArgs, CancellationToken cancellationToken = default);
}
