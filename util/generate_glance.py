import os
import sys
import json
import io
from pathlib import Path
import difflib
import shutil
import glob
import tarfile
import zipfile
import stat
import subprocess
import csv
import collections
import numpy as np
import cv2


def generate_glance(folder: str, out_folder: str):
    mesh_folder = os.path.join(folder, 'meshes')
    allobjs_json_file = os.path.join(mesh_folder, 'allObjs.json')

    for file in glob.glob(os.path.join(folder, '*.glance')):
        shutil.copy2(file, out_folder)

    # for file in glob.glob(os.path.join(mesh_folder, '*.msh.vtp')):
    #     shutil.copy2(file, out_folder)

    template_json_file = os.path.join(folder, 'state_pc_template.json')
    with open(template_json_file, mode='r', encoding='utf-8') as jf:
        template = json.load(jf)
    with open(allobjs_json_file, mode='r', encoding='utf-8') as jf:
        allobjs = json.load(jf)
    for obj in allobjs:
        template['sources'][0]['props']['name'] = 'neurites_' + obj['cellname']
        template['sources'][0]['props']['dataset']['name'] = obj['neurite']
        template['sources'][0]['props']['dataset']['url'] = '/static/data/' + obj['neurite']
        template['sources'][1]['props']['name'] = 'soma_' + obj['cellname']
        template['sources'][1]['props']['dataset']['name'] = obj['soma']
        template['sources'][1]['props']['dataset']['url'] = '/static/data/' + obj['soma']
        template['sources'][2]['props']['name'] = 'synapses_' + obj['cellname']
        template['sources'][2]['props']['dataset']['name'] = obj['puncta']
        template['sources'][2]['props']['dataset']['url'] = '/static/data/' + obj['puncta']

        template['views'][0]['camera']['position'] = obj['camera position']
        template['views'][0]['camera']['focalPoint'] = obj['camera focalPoint']

        glance_state_file = os.path.join(folder, 'state.json')
        with open(glance_state_file, mode='w', encoding='utf-8') as outfile:
            json.dump(template, outfile)

        glance_out_file = os.path.join(out_folder, obj['cellname'] + '.glance')

        with zipfile.ZipFile(glance_out_file, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.write(glance_state_file, 'state.json')

        # for fn in [obj['neurite'], obj['soma'], obj['puncta'], obj['root']]:
        #     shutil.copy2(os.path.join(mesh_folder, fn), out_folder)


def generate_thumbnails(folder: str, out_folder: str):
    for file in glob.glob(os.path.join(folder, '*.jpeg')):
        im = cv2.imread(file)
        fn = Path(file).stem
        cv2.imwrite(os.path.join(out_folder, fn + '.jpg'), im)

    for file in glob.glob(os.path.join(folder, 'thumbnails', '*.tif')):
        im = cv2.imread(file)
        fn = Path(file).stem
        cv2.imwrite(os.path.join(out_folder, fn + '.jpg'), im)


def convert_pdf_to_img(pdfs, imgs):
    for i in range(len(pdfs)):
        if not os.path.exists(pdfs[i]):
            print('file', pdfs[i], 'does not exist')
            continue
        print(imgs[i])
        if 'pattern' in imgs[i]:
            subprocess.run(['gs', '-sDEVICE=png16m', '-r300', '-dTextAlphaBits=4', '-o', imgs[i] + '.png', pdfs[i]],
                           shell=False, check=True)
        else:
            subprocess.run(['gs', '-sDEVICE=png16m', '-r300', '-dTextAlphaBits=4', '-o', imgs[i] + '.png', pdfs[i]],
                           shell=False, check=True)

        img = cv2.imread(imgs[i] + '.png')  # Read in the image and convert to grayscale
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        gray = 255 * (gray < 128).astype(np.uint8)  # To invert the text to white
        coords = cv2.findNonZero(gray)  # Find all non-zero points (text)
        x, y, w, h = cv2.boundingRect(coords)  # Find minimum spanning bounding box
        rect = img[y:y + h, x:x + w]  # Crop the image - note we do this on the original image
        top = int(0.05 * rect.shape[0])  # shape[0] = rows
        bottom = top
        left = int(0.05 * rect.shape[1])  # shape[1] = cols
        right = left
        value = [255, 255, 255]
        rect1 = cv2.copyMakeBorder(rect, top, bottom, left, right, cv2.BORDER_CONSTANT, None, value)
        cv2.imwrite(imgs[i] + '.png', rect1)  # Save the image


def generate_analysis_figures(folder: str, out_folder: str):
    cells = []
    with open('/Users/feng/code/mgrasp-analysis/pv_figs_1.0/orig2_cell_props.csv', newline='') as csvfile:
        reader = csv.DictReader(csvfile)
        for row in reader:
            cellname = row['CellName']
            print(cellname)
            res_folder = os.path.join(out_folder, cellname)
            if not os.path.exists(res_folder):
                os.mkdir(res_folder)

            cell = collections.OrderedDict()
            cell['name'] = cellname
            cell_summary = collections.OrderedDict()

            cell_summary['name'] = row['CellName']
            cell_summary['animal name'] = row['AnimalName']

            cell_summary['cell type'] = row['CellType']
            if cell_summary['cell type']  == 'Pyr':
                cell_summary['cell type'] = 'pyramidal cell (PC)'
                cell_summary['input region'] = 'left CA3'
                cell_summary['cell location'] = 'right CA1'
            elif cell_summary['cell type']  == 'contraPV':
                cell_summary['cell type'] = 'parvalbumin-positive interneuron (PV+)'
                cell_summary['input region'] = 'left CA3'
                cell_summary['cell location'] = 'right CA1'
            elif cell_summary['cell type']  == 'ipsiPV':
                cell_summary['cell type'] = 'parvalbumin-positive interneuron (PV+)'
                cell_summary['input region'] = 'right CA3'
                cell_summary['cell location'] = 'right CA1'

            cell_summary['soma strata location'] = row['SomaLocation']
            if cell_summary['soma strata location'] == 'Ori':
                cell_summary['soma strata location'] = 'stratum oriens (SO)'
            elif cell_summary['soma strata location'] == 'Rad':
                cell_summary['soma strata location'] = 'stratum radiatum (SR)'
            elif cell_summary['soma strata location'] == 'Deep':
                cell_summary['soma strata location'] = 'stratum pyramidale (SP) deep'
            elif cell_summary['soma strata location'] == 'Superficial':
                cell_summary['soma strata location'] = 'stratum pyramidale (SP) superficial'

            cell_summary['synapse number'] = row['SynapseNumber']
            cell_summary['dendrites surface area (um2)'] = row['BranchSurfaceArea']
            cell_summary['dendrites volume (um3)'] = row['NeuriteVolume']
            cell_summary['dendrites length (um)'] = row['NeuriteLength']
            cell_summary['synapse density (#/um2)'] = row['SynapseDensity']

            cell['summary'] = cell_summary

            cell_analyses = []

            analysis = collections.OrderedDict()
            analysis['name'] = 'dendrogram'
            analysis['url'] = '/static/analysis/' + cellname + '/dendrogram.png'
            analysis['description'] = 'Dendrogram shows the hierarchical tree structure of the neuron and the ' \
                                      'distribution of synapses on individual dendrite. Thin lines indicate dendrite\
                                       portotions in alveus (alv), Stratum pyramidale (SP), or Stratum lacunosum ' \
                                      'moleculare (SLM) where little or no axonal input was found.'
            cell_analyses.append(analysis)

            analysis = collections.OrderedDict()
            analysis['name'] = 'sholl analysis'
            analysis['url'] = '/static/analysis/' + cellname + '/sholl_analysis.png'
            analysis['description'] = 'Sholl analysis is a method of quantitative analysis commonly used in neuronal' \
                                      ' studies to characterize the morphological characteristics of a neuron. It uses' \
                                      ' a series of concentric spherical shells as coordinates of reference to study' \
                                      ' how the number of dendrites and synapses varies with the distance from the soma.'
            cell_analyses.append(analysis)

            analysis = collections.OrderedDict()
            analysis['name'] = 'dendrite-level synapse pattern'
            analysis['url'] = '/static/analysis/' + cellname + '/overall_synapse_pattern.png'
            analysis['description'] = 'Dendrite-level synapse pattern analysis shows the variability in the synaptic' \
                                      ' density of individual dendrites of a given neuron by comparing the measured' \
                                      ' synapse numbers with those expected by the random null model, i.e., the number' \
                                      ' of synapses that would be predicted by a Poisson process with a fixed density' \
                                      ' per unit dendrite area.'
            cell_analyses.append(analysis)

            if cell['summary']['cell type'] == 'pyramidal cell (PC)':
                analysis = collections.OrderedDict()
                analysis['name'] = 'basal dendrite synapse pattern'
                analysis['url'] = '/static/analysis/' + cellname + '/basal_pattern.png'
                analysis['description'] = 'Dendrite-level synapse pattern analysis for only basal dendrites'
                cell_analyses.append(analysis)

                analysis = collections.OrderedDict()
                analysis['name'] = 'apical dendrite synapse pattern'
                analysis['url'] = '/static/analysis/' + cellname + '/apical_pattern.png'
                analysis['description'] = 'Dendrite-level synapse pattern analysis for only apical dendrites'
                cell_analyses.append(analysis)

            analysis = collections.OrderedDict()
            analysis['name'] = 'sholl analysis with laminar correction'
            analysis['url'] = '/static/analysis/' + cellname + '/layer_sholl_analysis.png'
            analysis['description'] = 'In this dataset, little or no axonal input was found in alveus (alv), ' \
                                      'Stratum pyramidale (SP), and Stratum lacunosum moleculare (SLM). ' \
                                      'To analyze dendrite-level synaptic connectivity more accurately, ' \
                                      'we removed the dendrite portions in these regions. Only dendrite portions ' \
                                      'in Stratum oriens (SO) and Stratum radiatum (SR) were analyzed.'
            cell_analyses.append(analysis)

            analysis = collections.OrderedDict()
            analysis['name'] = 'dendrite-level synapse pattern with laminar correction'
            analysis['url'] = '/static/analysis/' + cellname + '/layer_overall_synapse_pattern.png'
            analysis['description'] = 'In this dataset, little or no axonal input was found in alveus (alv), ' \
                                      'Stratum pyramidale (SP), and Stratum lacunosum moleculare (SLM). ' \
                                      'To analyze dendrite-level synaptic connectivity more accurately, ' \
                                      'we removed the dendrite portions in these regions. Only dendrite portions ' \
                                      'in Stratum oriens (SO) and Stratum radiatum (SR) were analyzed.'
            cell_analyses.append(analysis)

            cell['analyses'] = cell_analyses

            cells.append(cell)

    with open(os.path.join(out_folder, 'all.json'), 'w') as outfile:
        json.dump(cells, outfile)

    mesh_folder = os.path.join(folder, 'meshes')
    allobjs_json_file = os.path.join(mesh_folder, 'allObjs.json')

    with open(allobjs_json_file, mode='r', encoding='utf-8') as jf:
        allobjs = json.load(jf)
    for obj in allobjs:
        cellname = obj['cellname']
        res_folder = os.path.join(out_folder, cellname)
        if not os.path.exists(res_folder):
            continue

        pdf_folder = os.path.join(os.path.expanduser('~'), 'code', 'mgrasp-analysis', 'pv_figs_11')

        celltype = ''
        if cellname.startswith('PV'):
            celltype = 'contra'
            if not os.path.exists(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_layer_fig1.pdf')):
                celltype = 'ipsi'
                if not os.path.exists(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_layer_fig1.pdf')):
                    continue
        else:
            celltype = 'py'
            if not os.path.exists(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_layer_fig1.pdf')):
                continue

        pdfs = []
        imgs = []
        pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_layer_fig1.pdf'))
        imgs.append(os.path.join(res_folder, 'layer_overall_synapse_pattern'))

        pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_layer_fig2.pdf'))
        imgs.append(os.path.join(res_folder, 'layer_sholl_analysis'))

        pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_fig1.pdf'))
        imgs.append(os.path.join(res_folder, 'overall_synapse_pattern'))

        pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_fig2.pdf'))
        imgs.append(os.path.join(res_folder, 'sholl_analysis'))

        pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_dendrogram.pdf'))
        imgs.append(os.path.join(res_folder, 'dendrogram'))

        if celltype == 'py':
            pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_basal_fig1.pdf'))
            imgs.append(os.path.join(res_folder, 'basal_pattern'))
            pdfs.append(os.path.join(pdf_folder, 'all', celltype + '_' + cellname + '_apical_fig1.pdf'))
            imgs.append(os.path.join(res_folder, 'apical_pattern'))

        # if celltype == 'py':
        #     celltype = 'pyr'
        # pdfs.append(os.path.join(pdf_folder, 'distden', celltype + '_' + cellname + '_distden.pdf'))
        # imgs.append(os.path.join(res_folder, 'distden'))
        #
        # if celltype == 'pyr':
        #     celltype = 'layer_Pyr'
        # if celltype == 'contra':
        #     celltype = 'layer_contraPV'
        # if celltype == 'ipsi':
        #     celltype = 'layer_ipsiPV'
        # pdfs.append(os.path.join(pdf_folder, 'IndividualCellBranchLevelPattern',
        #                          celltype + '_' + cellname + '_branch_level_pattern.pdf'))
        # imgs.append(os.path.join(res_folder, 'branch_level_pattern'))
        #
        # pdfs.append(os.path.join(pdf_folder, 'IndividualCellBranchLevelPattern_length',
        #                          celltype + '_' + cellname + '_branch_level_pattern.pdf'))
        # imgs.append(os.path.join(res_folder, 'branch_level_pattern_length'))

        convert_pdf_to_img(pdfs, imgs)


if __name__ == "__main__":
    # generate_glance(os.path.join(os.path.expanduser('~'), 'Google Drive', 'lab', 'eeum', 'raw'),
    #                 os.path.join(os.path.expanduser('~'), 'Google Drive', 'lab', 'eeum', 'static', 'data'))
    # generate_thumbnails(os.path.join(os.path.expanduser('~'), 'Google Drive', 'lab', 'eeum', 'raw'),
    #                     os.path.join(os.path.expanduser('~'), 'Google Drive', 'lab', 'eeum', 'static', 'data'))
    generate_analysis_figures(os.path.join(os.path.expanduser('~'), 'Google Drive', 'lab', 'eeum', 'raw'),
                              os.path.join(os.path.expanduser('~'), 'Google Drive', 'lab', 'eeum', 'static', 'analysis'))
