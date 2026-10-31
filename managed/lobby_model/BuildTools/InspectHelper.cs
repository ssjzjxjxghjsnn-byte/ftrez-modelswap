using System;
using System.Linq;
using Mono.Cecil;

static class InspectHelper
{
    static void Main(string[] args)
    {
        using (var assembly = AssemblyDefinition.ReadAssembly(args[0]))
        {
            var entry = assembly.MainModule.Types.Single(t => t.FullName == "FTRez.LobbyModel.Entry");
            Console.WriteLine("revision\t" + entry.Fields.Single(f => f.Name == "Revision").Constant);
            foreach (var method in entry.Methods.Where(m => m.IsPublic && m.IsStatic))
                Console.WriteLine("method\t" + method.Name + "\t" + method.ReturnType.FullName + "\t" +
                    string.Join(",", method.Parameters.Select(p => p.ParameterType.FullName)));
        }
    }
}
