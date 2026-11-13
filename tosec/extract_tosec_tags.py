import re
import os

def extract_tosec_tags(file_path):
    if not os.path.exists(file_path):
        print(f"Error: {file_path} not found.")
        return

    # Patterns for:
    # [m ...] - Modified
    # [cr ...] - Cracked
    # [h ...] - Hacked
    # [t ...] or [tr ...] - Trainer (normalized to [t ...])
    m_pattern = re.compile(r'\[m ([^\]]+)\]')
    cr_pattern = re.compile(r'\[cr ([^\]]+)\]')
    h_pattern = re.compile(r'\[h ([^\]]+)\]')
    t_pattern = re.compile(r'\[t(?:r)? ([^\]]+)\]')

    categories = {
        "Modified [m ...]": set(),
        "Cracked [cr ...]": set(),
        "Hacked [h ...]": set(),
        "Trainer [t ... / tr ...]": set()
    }
    
    multiple_tags_count = 0

    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            for line in f:
                m_matches = m_pattern.findall(line)
                cr_matches = cr_pattern.findall(line)
                h_matches = h_pattern.findall(line)
                t_matches = t_pattern.findall(line)

                for match in m_matches: categories["Modified [m ...]"].add(match)
                for match in cr_matches: categories["Cracked [cr ...]"].add(match)
                for match in h_matches: categories["Hacked [h ...]"].add(match)
                for match in t_matches: categories["Trainer [t ... / tr ...]"].add(match)

                # Total count of all these specific tags on the line
                total_matches = len(m_matches) + len(cr_matches) + len(h_matches) + len(t_matches)
                if total_matches > 1:
                    multiple_tags_count += 1

    except Exception as e:
        print(f"Error reading file: {e}")
        return

    for label, tags in categories.items():
        print(f"\n{label}:")
        if not tags:
            print("  None found.")
        else:
            for tag in sorted(tags):
                # Using a generic bracket for display
                prefix = label.split('[')[1].split(']')[0].split(' ')[0]
                print(f"  [{prefix} {tag}]")
    
    print(f"\nTotal lines/entries with multiple tags (m, cr, h, t/tr): {multiple_tags_count}")

if __name__ == "__main__":
    tosec_file = os.path.join(os.path.dirname(__file__), "tosec.json")
    extract_tosec_tags(tosec_file)
