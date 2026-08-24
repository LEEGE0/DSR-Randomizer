namespace DSRRandomizer.Foundation.Runtime;

public interface IClock
{
    DateTimeOffset UtcNow { get; }
}
