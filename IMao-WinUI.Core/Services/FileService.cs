using System.Text;

using IMao_WinUI.Core.Contracts.Services;

using Newtonsoft.Json;

namespace IMao_WinUI.Core.Services;

public class FileService : IFileService
{
    public T Read<T>(string folderPath, string fileName)
    {
        var path = Path.Combine(folderPath, fileName);
        if (File.Exists(path))
        {
            var json = File.ReadAllText(path);
            try { return JsonConvert.DeserializeObject<T>(json); }
            catch (JsonException)
            {
                // Preserve the damaged bytes before falling back to defaults.
                File.Copy(path, path + ".corrupt-" + Guid.NewGuid().ToString("N"));
                return default;
            }
        }

        return default;
    }

    public void Save<T>(string folderPath, string fileName, T content)
    {
        if (!Directory.Exists(folderPath))
        {
            Directory.CreateDirectory(folderPath);
        }

        var fileContent = JsonConvert.SerializeObject(content);
        Helpers.AtomicFile.WriteAllText(Path.Combine(folderPath, fileName), fileContent);
    }

    public void Delete(string folderPath, string fileName)
    {
        if (fileName != null && File.Exists(Path.Combine(folderPath, fileName)))
        {
            File.Delete(Path.Combine(folderPath, fileName));
        }
    }
}
